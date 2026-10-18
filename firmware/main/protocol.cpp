#include "protocol.h"
#include "board.h"
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <tusb.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cmath>
#include <memory>
#include <algorithm>
#include <cstddef>

namespace otp {
namespace {
struct alignas(max_align_t) Allocation { size_t length; };
void* secure_alloc(size_t n) {
    if (n > 16384) return nullptr;
    auto* a = static_cast<Allocation*>(heap_caps_malloc(sizeof(Allocation) + n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!a) return nullptr;
    a->length = n; return a + 1;
}
void secure_free(void* p) {
    if (!p) return;
    auto* a = static_cast<Allocation*>(p) - 1; wipe(p,a->length); free(a);
}
const char* str(cJSON* o, const char* key) {
    auto* item = cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(item) ? item->valuestring : nullptr;
}
bool integer(cJSON* o, const char* key, int64_t& value, int64_t lo, int64_t hi) {
    auto* item = cJSON_GetObjectItemCaseSensitive(o,key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < lo || item->valuedouble > hi) return false;
    value = static_cast<int64_t>(item->valuedouble); return true;
}
bool clean_tree(cJSON* o, int depth = 0) {
    if (depth > 6) return false;
    for (auto* p = o->child; p; p = p->next) {
        if (cJSON_IsObject(o)) for (auto* q = p->next; q; q = q->next)
            if (p->string && q->string && !strcmp(p->string,q->string)) return false;
        if (!clean_tree(p,depth+1)) return false;
    }
    return true;
}
cJSON* object() { return cJSON_CreateObject(); }
bool parse_record(cJSON* o, Record& r) {
    if (!cJSON_IsObject(o)) return false;
    const char *id=str(o,"id"), *issuer=str(o,"issuer"), *account=str(o,"account"), *secret=str(o,"secret"), *algo=str(o,"algorithm");
    int64_t digits, period;
    if (!id || !issuer || !account || !secret || !algo || strlen(id) >= sizeof r.id ||
        strlen(issuer) >= sizeof r.issuer || strlen(account) >= sizeof r.account || strlen(secret) > 256 ||
        !integer(o,"digits",digits,6,8) || !integer(o,"period",period,30,60)) return false;
    strcpy(r.id,id); strcpy(r.issuer,issuer); strcpy(r.account,account);
    r.algorithm = !strcmp(algo,"SHA1") ? Algorithm::Sha1 : !strcmp(algo,"SHA256") ? Algorithm::Sha256 :
        !strcmp(algo,"SHA512") ? Algorithm::Sha512 : static_cast<Algorithm>(0);
    r.digits = digits; r.period = period; size_t length;
    if (!base32_decode(secret,r.secret,sizeof r.secret,length)) return false;
    r.secret_len = length; return valid_record(r);
}
cJSON* record_json(const Record& r, bool secret) {
    auto* o=object(); cJSON_AddStringToObject(o,"id",r.id); cJSON_AddStringToObject(o,"issuer",r.issuer);
    cJSON_AddStringToObject(o,"account",r.account); cJSON_AddStringToObject(o,"algorithm",algorithm_name(r.algorithm));
    cJSON_AddNumberToObject(o,"digits",r.digits); cJSON_AddNumberToObject(o,"period",r.period);
    if (secret) { auto s=base32_encode(r.secret,r.secret_len); cJSON_AddStringToObject(o,"secret",s.c_str()); wipe(s.data(),s.size()); }
    return o;
}
}
bool Protocol::begin() {
    ui_.set_lock_hook([this]{
        clear_transfers();authorized_=false;
        wipe(line_,sizeof line_);line_size_=0;overflow_=false;
    });
    cJSON_Hooks hooks{secure_alloc,secure_free}; cJSON_InitHooks(&hooks);
    tinyusb_config_t usb{}; usb.device_descriptor = nullptr; usb.string_descriptor = nullptr;
    usb.external_phy = false; usb.configuration_descriptor = nullptr;
    if (tinyusb_driver_install(&usb) != ESP_OK) return false;
    tinyusb_config_cdcacm_t cdc{}; cdc.usb_dev = TINYUSB_USBDEV_0; cdc.cdc_port = TINYUSB_CDC_ACM_0;
    return tusb_cdc_acm_init(&cdc) == ESP_OK;
}
bool Protocol::send(const char* text) {
    if (!tud_cdc_connected()) return false;
    size_t length = strlen(text); if (length + 1 > sizeof line_) return false;
    size_t n = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,reinterpret_cast<const uint8_t*>(text),length);
    const uint8_t newline='\n'; n += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,&newline,1);
    return n == length+1 && tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,pdMS_TO_TICKS(200)) == ESP_OK;
}
void Protocol::reply(int id, cJSON* result) {
    auto* o=object(); cJSON_AddNumberToObject(o,"v",1); cJSON_AddNumberToObject(o,"id",id); cJSON_AddBoolToObject(o,"ok",true);
    cJSON_AddItemToObject(o,"result",result ? result : object()); auto* text=cJSON_PrintUnformatted(o);
    if (text) { send(text); cJSON_free(text); } cJSON_Delete(o);
}
void Protocol::error(int id, const char* code, const char* message) {
    auto* o=object(); cJSON_AddNumberToObject(o,"v",1); cJSON_AddNumberToObject(o,"id",id); cJSON_AddBoolToObject(o,"ok",false);
    auto* e=object(); cJSON_AddStringToObject(e,"code",code); cJSON_AddStringToObject(e,"message",message); cJSON_AddItemToObject(o,"error",e);
    auto* text=cJSON_PrintUnformatted(o); if (text) { send(text);cJSON_free(text); } cJSON_Delete(o);
}
void Protocol::clear_transfers() {
    wipe(staging_.data(),sizeof staging_); staged_=expected_=export_index_=0; restoring_=exporting_=false;
}
void Protocol::disconnected() {
    authorized_=false; ui_.cancel(); pending_=false; clear_transfers(); wipe(line_,sizeof line_); line_size_=0; overflow_=false;
    // Discard any unread bytes: reconnect never inherits the previous host's messages.
    uint8_t bytes[128]; size_t n=0;
    do { tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0,bytes,sizeof bytes,&n); wipe(bytes,sizeof bytes); } while(n);
}
void Protocol::status(int id) {
    auto* o=object(); cJSON_AddStringToObject(o,"firmware","0.1.0");
    const char* state = vault_.status()==VaultStatus::Empty?"empty":vault_.status()==VaultStatus::Locked?"locked":
        vault_.status()==VaultStatus::Unlocked?"unlocked":vault_.status()==VaultStatus::Damaged?"damaged":"storage_error";
    cJSON_AddStringToObject(o,"state",state); cJSON_AddBoolToObject(o,"authorized",authorized_ && vault_.status()==VaultStatus::Unlocked);
    cJSON_AddBoolToObject(o,"timeValid",board::rtc_valid()); cJSON_AddNumberToObject(o,"time",board::rtc_valid()?board::now():0);
    cJSON_AddNumberToObject(o,"batteryMv",board::battery_mv()); cJSON_AddNumberToObject(o,"retryAfter",vault_.wait_seconds()); reply(id,o);
}
void Protocol::receive(char* line) {
    // Bound recursion before invoking cJSON's recursive parser.
    int depth=0; bool in_string=false, escape=false;
    for (char* p=line; *p; ++p) {
        if (in_string) { if(escape) escape=false; else if(*p=='\\') escape=true; else if(*p=='"') in_string=false; }
        else if(*p=='"') in_string=true;
        else if(*p=='{' || *p=='[') { if(++depth>6) { error(0,"FRAME","Слишком глубокое сообщение"); return; } }
        else if(*p=='}' || *p==']') --depth;
    }
    auto root=std::shared_ptr<cJSON>(cJSON_ParseWithLengthOpts(line,strlen(line)+1,nullptr,true),cJSON_Delete);
    int64_t v,id64;
    if (!root || !cJSON_IsObject(root.get()) || !clean_tree(root.get()) || !integer(root.get(),"v",v,1,1) ||
        !integer(root.get(),"id",id64,1,2147483647)) { error(0,"FRAME","Неверное сообщение протокола"); return; }
    int id=id64; const char* op=str(root.get(),"op"); auto* args=cJSON_GetObjectItemCaseSensitive(root.get(),"args");
    if (!op || !cJSON_IsObject(args)) { error(id,"FRAME","Неверная команда"); return; }
    if (!strcmp(op,"status")) { status(id); return; }
    if (!strcmp(op,"lock")) { ui_.lock(); clear_transfers(); reply(id,object()); return; }
    if (vault_.status()!=VaultStatus::Unlocked || !authorized_) { error(id,"LOCKED","Введите PIN и разрешите USB на StopWatch"); return; }
    if (pending_) { error(id,"BUSY","Подтвердите действие на StopWatch"); return; }
    transfer_activity_=esp_timer_get_time();
    if (!strcmp(op,"list")) {
        int64_t offset;
        if (!integer(args,"offset",offset,0,MaxRecords)) { error(id,"INPUT","Неверный номер страницы"); return; }
        auto* o=object(); auto* rows=cJSON_CreateArray();
        size_t end=std::min(vault_.count(),static_cast<size_t>(offset)+5);
        for(size_t i=offset;i<end;++i) cJSON_AddItemToArray(rows,record_json(vault_.record(i),false));
        cJSON_AddNumberToObject(o,"total",vault_.count()); cJSON_AddItemToObject(o,"records",rows); reply(id,o); return;
    }
    if (!strcmp(op,"restore_begin")) {
        int64_t count;
        if (!integer(args,"count",count,0,MaxRecords)) { error(id,"INPUT","Лимит: 100 аккаунтов");return; }
        clear_transfers(); restoring_=true;expected_=count;reply(id,object());return;
    }
    if (!strcmp(op,"restore_item")) {
        int64_t index; Record r{};
        bool ok=restoring_ && integer(args,"index",index,0,MaxRecords-1) && static_cast<size_t>(index)==staged_ && staged_<expected_ &&
            parse_record(cJSON_GetObjectItemCaseSensitive(args,"record"),r);
        if (ok) for(size_t i=0;i<staged_;++i) if(!strcmp(staging_[i].id,r.id)) ok=false;
        if (ok) { staging_[staged_++]=r; reply(id,object()); } else { clear_transfers();error(id,"INPUT","Восстановление прервано: неверная запись"); }
        wipe(&r,sizeof r);return;
    }
    if (!strcmp(op,"restore_abort")) { clear_transfers(); reply(id,object());return; }
    if (!strcmp(op,"export_next")) {
        if (!exporting_ || export_index_>=vault_.count()) { error(id,"STATE","Экспорт недоступен");return; }
        auto* o=object();cJSON_AddItemToObject(o,"record",record_json(vault_.record(export_index_++),true));
        if(export_index_==vault_.count()) exporting_=false;
        reply(id,o);return;
    }
    std::string question;
    const int64_t received_at=esp_timer_get_time();
    if (!strcmp(op,"upsert")) {
        Record r{}; bool valid=parse_record(cJSON_GetObjectItemCaseSensitive(args,"record"),r);
        if (!valid) { wipe(&r,sizeof r);error(id,"INPUT","Неверные параметры аккаунта");return; }
        question=std::string("Сохранить аккаунт?\n")+r.issuer+"\n"+r.account+"\n"+algorithm_name(r.algorithm)+" / "+std::to_string(r.digits)+" / "+std::to_string(r.period)+" с";
        wipe(&r,sizeof r);
    } else if (!strcmp(op,"rename")) {
        auto* recid=str(args,"id"); auto* issuer=str(args,"issuer");auto* account=str(args,"account");
        if(!recid || !issuer || !account || strlen(issuer)>96 || strlen(account)>128) { error(id,"INPUT","Неверное название");return; }
        bool valid=false;
        for(size_t i=0;i<vault_.count();++i) if(!strcmp(vault_.record(i).id,recid)) {
            Record r=vault_.record(i);strcpy(r.issuer,issuer);strcpy(r.account,account);valid=valid_record(r);wipe(&r,sizeof r);break;
        }
        if(!valid) {error(id,"INPUT","Неверное название или неизвестный аккаунт");return;}
        question=std::string("Переименовать?\n")+issuer+"\n"+account;
    } else if (!strcmp(op,"delete")) {
        const char* recid=str(args,"id"); const Record* found=nullptr;
        if(recid) for(size_t i=0;i<vault_.count();++i) if(!strcmp(vault_.record(i).id,recid)) found=&vault_.record(i);
        if(!found) { error(id,"INPUT","Аккаунт не найден");return; }
        question=std::string("Удалить аккаунт?\n")+found->issuer+"\n"+found->account;
    } else if (!strcmp(op,"time_set")) {
        int64_t epoch;if(!integer(args,"epoch",epoch,946684800,4102444799LL)) { error(id,"INPUT","Неверное время UTC");return; }
        time_t t=epoch;tm utc{};
        #ifdef _WIN32
        gmtime_s(&utc,&t);
        #else
        gmtime_r(&t,&utc);
        #endif
        char date[48];strftime(date,sizeof date,"%Y-%m-%d\n%H:%M:%S UTC",&utc);
        question=std::string("Установить время?\n")+date;
        // Apply elapsed confirmation time, rather than the stale timestamp from request arrival.
    } else if (!strcmp(op,"export_begin")) {
        clear_transfers();question="Передать все секреты\nэтому компьютеру?\nСоздавайте копию только\nна доверенном ПК.";
    } else if (!strcmp(op,"restore_commit")) {
        if(!restoring_ || staged_!=expected_) { error(id,"STATE","Получены не все аккаунты");return; }
        question="Заменить ВСЕ аккаунты\nданными резервной копии?\nАккаунтов: "+std::to_string(staged_);
    } else { error(id,"UNKNOWN","Неизвестная команда");return; }
    pending_=true;
    bool asked=ui_.ask(question,[this,root,id,received_at](bool yes) {
        pending_=false;auto* args=cJSON_GetObjectItemCaseSensitive(root.get(),"args");const char* op=str(root.get(),"op");
        if(!yes || !authorized_ || vault_.status()!=VaultStatus::Unlocked || !tud_cdc_connected()) {
            clear_transfers();error(id,"CANCELLED","Действие отменено на устройстве");return;
        }
        transfer_activity_=esp_timer_get_time();
        bool ok=false;
        if(!strcmp(op,"upsert")) { Record r{};ok=parse_record(cJSON_GetObjectItemCaseSensitive(args,"record"),r)&&vault_.upsert(r);wipe(&r,sizeof r); }
        else if(!strcmp(op,"delete")) ok=vault_.remove(str(args,"id"));
        else if(!strcmp(op,"rename")) {
            const char* recid=str(args,"id");
            for(size_t i=0;i<vault_.count();++i) if(!strcmp(vault_.record(i).id,recid)) {
                Record r=vault_.record(i);strcpy(r.issuer,str(args,"issuer"));strcpy(r.account,str(args,"account"));ok=vault_.upsert(r);wipe(&r,sizeof r);break;
            }
        } else if(!strcmp(op,"time_set")) {
            int64_t epoch=static_cast<int64_t>(cJSON_GetObjectItemCaseSensitive(args,"epoch")->valuedouble);
            ok=board::set_time(epoch+(esp_timer_get_time()-received_at)/1000000);
        } else if(!strcmp(op,"export_begin")) {
            exporting_=vault_.count()>0;export_index_=0;auto* o=object();cJSON_AddNumberToObject(o,"count",vault_.count());reply(id,o);return;
        } else if(!strcmp(op,"restore_commit")) { ok=vault_.replace(staging_.data(),staged_);clear_transfers(); }
        if(ok) reply(id,object());else error(id,"FAILED",!strcmp(op,"time_set")?"Не удалось записать RTC":vault_.error());
        ui_.refresh();
    });
    if(!asked) {pending_=false;error(id,"BUSY","На экране уже есть запрос");}
}
void Protocol::tick() {
    bool connected=tud_cdc_connected();
    if(connected!=was_connected_) { disconnected();was_connected_=connected; }
    if(!authorized_ || vault_.status()!=VaultStatus::Unlocked) clear_transfers();
    if(!pending_ && (restoring_ || exporting_) && esp_timer_get_time()-transfer_activity_>15000000) clear_transfers();
    if(line_size_ && esp_timer_get_time()-line_started_>3000000) {wipe(line_,sizeof line_);line_size_=0;overflow_=true;}
    if(!connected) return;
    uint8_t chunk[256];size_t n=0;
    if(tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0,chunk,sizeof chunk,&n)!=ESP_OK) return;
    for(size_t i=0;i<n;++i) {
        uint8_t b=chunk[i];
        if(b=='\n') {
            if(overflow_) error(0,"FRAME","Превышен размер или срок сообщения");
            else if(line_size_) {line_[line_size_]=0;receive(line_);}
            wipe(line_,sizeof line_);line_size_=0;overflow_=false;
        } else if(!overflow_) {
            if(!line_size_) line_started_=esp_timer_get_time();
            if(!b || line_size_>=sizeof line_-2) {wipe(line_,sizeof line_);line_size_=0;overflow_=true;}
            else line_[line_size_++]=static_cast<char>(b);
        }
    }
    wipe(chunk,sizeof chunk);
}
}
