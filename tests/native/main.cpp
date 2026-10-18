#include "vault.h"
#include "protocol.h"
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#ifdef NDEBUG
#error Native test assertions must remain enabled, including optimized builds.
#endif

static std::array<std::vector<uint8_t>,2> flash;
static esp_partition_t partitions[]={{0x80000,"vault_a",0},{0x80000,"vault_b",1}};
static int64_t clock_us=0;
static bool counter_exists=false, commit_fail=false;
static uint32_t counter=0,pending_counter=0;
static int cut_after=-1;
static bool usb_connected=false;
static int64_t applied_time=0;
static std::string usb_rx,usb_tx;
static std::function<void(bool)> confirm_action;
esp_err_t tinyusb_driver_install(const tinyusb_config_t*){return ESP_OK;}
esp_err_t tusb_cdc_acm_init(const tinyusb_config_cdcacm_t*){return ESP_OK;}
bool tud_cdc_connected(){return usb_connected;}
size_t tinyusb_cdcacm_write_queue(int,const uint8_t* p,size_t n){usb_tx.append(reinterpret_cast<const char*>(p),n);return n;}
esp_err_t tinyusb_cdcacm_write_flush(int,int){return ESP_OK;}
esp_err_t tinyusb_cdcacm_read(int,uint8_t* p,size_t cap,size_t* n){*n=std::min(cap,usb_rx.size());memcpy(p,usb_rx.data(),*n);usb_rx.erase(0,*n);return ESP_OK;}
namespace board {bool rtc_valid(){return true;}int64_t now(){return 1234567890;}bool set_time(int64_t t){applied_time=t;return true;}uint16_t battery_mv(){return 3900;}}
namespace otp {
bool Ui::ask(const std::string&,std::function<void(bool)> action){if(confirm_action)return false;confirm_action=std::move(action);return true;}
void Ui::cancel(){auto cb=std::move(confirm_action);confirm_action={};if(cb)cb(false);}
void Ui::lock(bool){usb_=false;cancel();if(on_lock_)on_lock_();vault_.lock();}
void Ui::refresh(){}
}
static void fresh() {for(auto& p:flash)p.assign(0x80000,255);counter_exists=false;counter=0;clock_us=0;cut_after=-1;commit_fail=false;}
const esp_partition_t* esp_partition_find_first(int,int,const char* name){for(auto& p:partitions)if(!strcmp(p.label,name))return &p;return nullptr;}
esp_err_t esp_partition_read(const esp_partition_t* p,size_t off,void* data,size_t n){if(off+n>p->size)return ESP_FAIL;memcpy(data,flash[p->slot].data()+off,n);return ESP_OK;}
static bool cut(){if(cut_after<0)return false;if(cut_after==0)return true;--cut_after;return false;}
esp_err_t esp_partition_write(const esp_partition_t* p,size_t off,const void* data,size_t n){
    if(off+n>p->size)return ESP_FAIL;bool fail=cut();size_t actual=fail?n/2:n;
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<actual;i++){assert((flash[p->slot][off+i]&bytes[i])==bytes[i]);flash[p->slot][off+i]&=bytes[i];}
    return fail?ESP_FAIL:ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t* p,size_t off,size_t n){if(off+n>p->size)return ESP_FAIL;bool fail=cut();std::fill_n(flash[p->slot].begin()+off,fail?n/2:n,255);return fail?ESP_FAIL:ESP_OK;}
esp_err_t nvs_flash_init(){return ESP_OK;}
esp_err_t nvs_open(const char*,int,nvs_handle_t* h){*h=1;return ESP_OK;}
esp_err_t nvs_get_u32(nvs_handle_t,const char*,uint32_t* value){if(!counter_exists)return ESP_ERR_NVS_NOT_FOUND;*value=counter;return ESP_OK;}
esp_err_t nvs_set_u32(nvs_handle_t,const char*,uint32_t value){pending_counter=value;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){if(commit_fail)return ESP_FAIL;counter=pending_counter;counter_exists=true;return ESP_OK;}
int64_t esp_timer_get_time(){return clock_us;}
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* data,size_t n){crc=~crc;for(size_t i=0;i<n;i++){crc^=data[i];for(int b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}return ~crc;}
void bootloader_random_enable(){}
void bootloader_random_disable(){}
void esp_fill_random(void* p,size_t n){std::random_device rng;auto* out=static_cast<uint8_t*>(p);for(size_t i=0;i<n;i++)out[i]=rng();}
static otp::Record record(int index=0){otp::Record r{};snprintf(r.id,sizeof r.id,"00000000-0000-4000-8000-%012d",index);strcpy(r.issuer,"PrivateMail");strcpy(r.account,"alice@example.com");memcpy(r.secret,"12345678901234567890",20);r.secret_len=20;return r;}
static void vectors(){
    const uint64_t times[]={59,1111111109,1111111111,1234567890,2000000000,20000000000ULL};
    const char* expected[3][6]={{"94287082","07081804","14050471","89005924","69279037","65353130"},
        {"46119246","68084774","67062674","91819424","90698825","77737706"},
        {"90693936","25091201","99943326","93441116","38618901","47863826"}};
    for(int a=0;a<3;a++){
        auto r=record();r.algorithm=static_cast<otp::Algorithm>(a+1);r.digits=8;r.secret_len=a==0?20:a==1?32:64;
        for(unsigned i=0;i<r.secret_len;i++)r.secret[i]='1'+(i%10);
        for(unsigned i=0;i<r.secret_len;i++)if(r.secret[i]==':')r.secret[i]='0';
        for(int i=0;i<6;i++){char code[9];assert(otp::totp(r,times[i],code));assert(!strcmp(code,expected[a][i]));}
    }
    auto r=record();char before[9],after[9];assert(otp::totp(r,29,before));assert(otp::totp(r,30,after));assert(strcmp(before,after));
    uint8_t out[128];size_t n;assert(otp::base32_decode("MZXW6YTBOI======",out,sizeof out,n));assert(n==6&&!memcmp(out,"foobar",6));
    for(auto bad:{"A","AAA","AAAAAA","MZ","MY=","MY======A","M1","MY======="})assert(!otp::base32_decode(bad,out,sizeof out,n));
    puts("PASS: RFC 6238 (18 vectors), boundary, Base32 rejection");
}
static void encryption(){
    fresh();otp::Vault v;assert(v.begin());assert(v.status()==otp::VaultStatus::Empty);assert(v.create("12345678"));
    auto r=record();assert(v.upsert(r));
    for(auto& p:flash){std::string memory(reinterpret_cast<char*>(p.data()),p.size());assert(memory.find("PrivateMail")==std::string::npos);assert(memory.find("12345678901234567890")==std::string::npos);}
    v.lock();assert(v.count()==0);assert(!v.unlock("87654321"));assert(v.unlock("12345678"));assert(v.count()==1);
    assert(v.change_pin("123456789012"));v.lock();assert(!v.unlock("12345678"));assert(v.unlock("123456789012"));
    std::array<otp::Record,100> many;for(int i=0;i<100;i++)many[i]=record(i);
    assert(v.replace(many.data(),many.size()));v.lock();assert(v.unlock("123456789012"));assert(v.count()==100);
    auto extra=record(101);assert(!v.upsert(extra));assert(v.remove(many[99].id));assert(v.count()==99);
    assert(v.reset());assert(v.status()==otp::VaultStatus::Empty);
    for(auto& p:flash)for(auto b:p)assert(b==255);
    puts("PASS: encrypted persistence, PIN change, 100 records, deletion, reset");
}
static std::vector<uint8_t> hex(const char* text){std::vector<uint8_t> result;for(size_t i=0;text[i];i+=2){unsigned b=0;sscanf(text+i,"%2x",&b);result.push_back(b);}return result;}
static void crypto_interop(){
    uint8_t salt[16],key[32],zero[32]{},nonce[12]{},plain[16]{},cipher[16]{},tag[16]{};
    for(unsigned i=0;i<16;i++)salt[i]=i;
    assert(otp::derive_key("12345678",salt,key));
    assert(!memcmp(key,hex("1f0be1be8f35627c8cdf3e7f396a01f5dac7da508e5830499d41f9b6d8baeafe").data(),32));
    assert(otp::encrypt(zero,nonce,nullptr,0,plain,16,cipher,tag));
    assert(!memcmp(cipher,hex("cea7403d4d606b6e074ec5d3baf39d18").data(),16));
    assert(!memcmp(tag,hex("d0d1c8a799996bf0265b98b5d48ab919").data(),16));
    tag[0]^=1;memset(plain,0xff,16);assert(!otp::decrypt(zero,nonce,nullptr,0,cipher,16,tag,plain));
    for(auto b:plain)assert(b==0);
    puts("PASS: PBKDF2/AES-GCM match independent Node/OpenSSL vectors; bad tag wipes output");
}
static void power_cuts(){
    for(int cut=0;cut<4;cut++){
        fresh();{otp::Vault v;assert(v.begin());assert(v.create("12345678"));assert(v.upsert(record()));cut_after=cut;assert(!v.upsert(record(1)));}
        cut_after=-1;otp::Vault reboot;assert(reboot.begin());assert(reboot.unlock("12345678"));assert(reboot.count()==1);assert(!strcmp(reboot.record(0).id,record().id));
    }
    fresh();{otp::Vault v;assert(v.begin());assert(v.create("12345678"));assert(v.upsert(record()));}
    // Corrupt newest ciphertext. CRC rejects it before trying the PIN, older snapshot survives.
    flash[1][150]^=1;
    {otp::Vault v;assert(v.begin());assert(v.unlock("12345678"));assert(v.count()==0);}
    flash[0][0]^=1;{otp::Vault v;assert(v.begin());assert(v.status()==otp::VaultStatus::Damaged);assert(!v.unlock("12345678"));}
    for(int cut=0;cut<8;cut++){
        fresh();otp::Vault v;assert(v.begin());assert(v.create("12345678"));assert(v.upsert(record()));
        cut_after=cut;assert(!v.change_pin("87654321"));cut_after=-1;
        const char* expected=cut<4?"12345678":"87654321";
        assert(v.unlock(expected));assert(v.count()==1);v.lock();
        otp::Vault reboot;assert(reboot.begin());assert(reboot.unlock(expected));assert(reboot.count()==1);
    }
    puts("PASS: cuts during erase/header/payload/commit, snapshot fallback, damaged vault");
}
static void retries(){
    fresh();otp::Vault v;assert(v.begin());assert(v.create("12345678"));v.lock();
    for(int i=0;i<5;i++)assert(!v.unlock("99999999"));assert(v.wait_seconds()==30);assert(!v.unlock("12345678"));
    clock_us+=29000000;otp::Vault reboot;assert(reboot.begin());assert(reboot.wait_seconds()==30);
    clock_us+=31000000;assert(reboot.unlock("12345678"));assert(reboot.wait_seconds()==0);assert(counter==0);
    reboot.lock();commit_fail=true;assert(!reboot.unlock("12345678"));assert(reboot.status()==otp::VaultStatus::StorageError);
    assert(otp::retry_delay(4)==0&&otp::retry_delay(5)==30&&otp::retry_delay(6)==60&&otp::retry_delay(12)==3600);
    puts("PASS: retry persistence, reboot delay, fail-closed counter writes");
}
static cJSON* rpc(otp::Protocol& p,const std::string& operation,const std::string& args="{}"){
    usb_tx.clear();usb_rx="{\"v\":1,\"id\":1,\"op\":\""+operation+"\",\"args\":"+args+"}\n";
    while(!usb_rx.empty())p.tick();return usb_tx.empty()?nullptr:cJSON_Parse(usb_tx.c_str());
}
static void confirm(bool yes){auto cb=std::move(confirm_action);confirm_action={};assert(cb);cb(yes);}
static bool ok(cJSON* r){bool value=r&&cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"));cJSON_Delete(r);return value;}
static void protocol_tests(){
    fresh();otp::Vault v;assert(v.begin());assert(v.create("12345678"));assert(v.upsert(record()));
    bool authorized=false;otp::Ui ui(v,authorized);otp::Protocol p(v,ui,authorized);assert(p.begin());
    usb_connected=true;p.tick();assert(ok(rpc(p,"status")));assert(!ok(rpc(p,"list","{\"offset\":0}")));
    authorized=true;auto* list=rpc(p,"list","{\"offset\":0}");assert(list);char* text=cJSON_PrintUnformatted(list);
    assert(!strstr(text,"secret"));cJSON_free(text);assert(ok(list));
    assert(!ok(rpc(p,"rename","{\"id\":\"00000000-0000-4000-8000-000000000000\",\"issuer\":\"bad\\nname\",\"account\":\"alice\"}")));
    assert(!confirm_action);
    assert(!rpc(p,"time_set","{\"epoch\":1234567890,\"receivedAt\":0}"));
    clock_us+=20000000;confirm(true);assert(applied_time==1234567910);assert(ok(cJSON_Parse(usb_tx.c_str())));
    assert(!rpc(p,"export_begin"));assert(confirm_action);assert(!ok(rpc(p,"export_next")));
    usb_tx.clear();confirm(false);assert(!ok(cJSON_Parse(usb_tx.c_str())));
    assert(!rpc(p,"export_begin"));clock_us+=20000000;p.tick(); // Waiting for confirmation must not expire a transfer.
    confirm(true);assert(ok(cJSON_Parse(usb_tx.c_str())));auto* exported=rpc(p,"export_next");
    assert(exported);auto* result=cJSON_GetObjectItem(exported,"result");auto* rec=cJSON_GetObjectItem(result,"record");
    assert(cJSON_IsString(cJSON_GetObjectItem(rec,"secret")));cJSON_Delete(exported);
    assert(!ok(rpc(p,"export_next")));
    assert(ok(rpc(p,"restore_begin","{\"count\":1}")));assert(!ok(rpc(p,"restore_commit")));assert(v.count()==1);
    assert(ok(rpc(p,"restore_begin","{\"count\":0}")));assert(!rpc(p,"restore_commit"));confirm(false);assert(v.count()==1);
    assert(ok(rpc(p,"restore_begin","{\"count\":0}")));assert(!rpc(p,"restore_commit"));clock_us+=20000000;p.tick();confirm(true);assert(v.count()==0);
    // Dropping DTR clears authorization and cancels an outstanding destructive request.
    assert(v.upsert(record()));assert(!rpc(p,"delete","{\"id\":\"00000000-0000-4000-8000-000000000000\"}"));
    usb_connected=false;p.tick();assert(!authorized&&!confirm_action);assert(v.count()==1);
    usb_connected=true;p.tick();assert(!ok(rpc(p,"list","{\"offset\":0}")));
    authorized=true;usb_tx.clear();usb_rx=std::string(5000,'x')+"\n{\"v\":1,\"id\":1,\"op\":\"status\",\"args\":{}}\n";
    while(!usb_rx.empty())p.tick();assert(usb_tx.find("FRAME")!=std::string::npos&&usb_tx.find("firmware")!=std::string::npos);
    assert(ok(rpc(p,"restore_begin","{\"count\":0}")));
    usb_rx="{\"v\":1,\"secret\":\"partial";p.tick();ui.lock();
    assert(v.unlock("12345678"));authorized=true;
    assert(ok(rpc(p,"status")));assert(!ok(rpc(p,"restore_commit")));assert(!confirm_action);
    puts("PASS: USB authorization, physical confirmation, paginated metadata, export, atomic restore, DTR reset, frame recovery");
}
int main(){setvbuf(stdout,nullptr,_IONBF,0);vectors();crypto_interop();encryption();power_cuts();retries();protocol_tests();puts("ALL NATIVE TESTS PASSED");}
