#include "vault.h"
#include "pattern.h"
#include "protocol.h"
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mbedtls/pkcs5.h>
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
static bool rtc_ok=true;
static std::string usb_rx,usb_tx;
static std::function<void(bool)> confirm_action;
esp_err_t tinyusb_driver_install(const tinyusb_config_t*){return ESP_OK;}
esp_err_t tusb_cdc_acm_init(const tinyusb_config_cdcacm_t*){return ESP_OK;}
bool tud_cdc_connected(){return usb_connected;}
size_t tinyusb_cdcacm_write_queue(int,const uint8_t* p,size_t n){usb_tx.append(reinterpret_cast<const char*>(p),n);return n;}
esp_err_t tinyusb_cdcacm_write_flush(int,int){return ESP_OK;}
esp_err_t tinyusb_cdcacm_read(int,uint8_t* p,size_t cap,size_t* n){*n=std::min(cap,usb_rx.size());memcpy(p,usb_rx.data(),*n);usb_rx.erase(0,*n);return ESP_OK;}
namespace board {bool rtc_valid(){return rtc_ok;}int64_t now(){return 1234567890;}bool set_time(int64_t t){applied_time=t;rtc_ok=true;return true;}uint16_t battery_mv(){return 3900;}}
namespace otp {
#ifndef STOPWATCH_UI_TESTS
bool Ui::ask(const std::string&,std::function<void(bool)> action){if(confirm_action)return false;confirm_action=std::move(action);return true;}
void Ui::cancel(){auto cb=std::move(confirm_action);confirm_action={};if(cb)cb(false);}
void Ui::lock(bool){usb_=false;cancel();if(on_lock_)on_lock_();vault_.lock();}
void Ui::refresh(){}
#endif
}
static void fresh() {for(auto& p:flash)p.assign(0x80000,255);counter_exists=false;counter=0;clock_us=0;cut_after=-1;commit_fail=false;rtc_ok=true;}
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
// Independent writer for the historical v1 layout. Production firmware cannot
// create PIN vaults; this fixture catches accidental changes to PIN migration.
#pragma pack(push,1)
struct LegacyHeader {
    uint32_t magic=0x3156544f,version=1; uint64_t generation=1;
    uint32_t count=1,length=sizeof(otp::Record),iterations=otp::KdfIterations;
    uint8_t salt[16]{},wrap_nonce[12]{},wrapped_key[32]{},wrap_tag[16]{},data_nonce[12]{},data_tag[16]{};
    uint32_t crc=0;
};
#pragma pack(pop)
static_assert(sizeof(LegacyHeader)==136,"v1 fixture layout changed");
static void legacy_fixture() {
    fresh(); LegacyHeader h; uint8_t key[32],kek[32],cipher[sizeof(otp::Record)];
    for(unsigned i=0;i<32;++i)key[i]=i+1;
    for(unsigned i=0;i<16;++i)h.salt[i]=i+10;
    auto r=record(); const char aad[]="StopWatch/DEK/v1";
    assert(mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        reinterpret_cast<const uint8_t*>("12345678"),8,h.salt,16,otp::KdfIterations,32,kek)==0);
    assert(otp::encrypt(kek,h.wrap_nonce,aad,sizeof aad,key,sizeof key,h.wrapped_key,h.wrap_tag));
    assert(otp::encrypt(key,h.data_nonce,&h,offsetof(LegacyHeader,data_tag),&r,sizeof r,cipher,h.data_tag));
    h.crc=esp_rom_crc32_le(0,reinterpret_cast<uint8_t*>(&h),offsetof(LegacyHeader,crc));
    h.crc=esp_rom_crc32_le(h.crc,cipher,sizeof cipher);
    memcpy(flash[0].data(),&h,sizeof h);memcpy(flash[0].data()+sizeof h,cipher,sizeof cipher);
    uint32_t marker=0x54494d43;memcpy(flash[0].data()+flash[0].size()-4,&marker,4);
    counter_exists=true;counter=0;otp::wipe(key,sizeof key);otp::wipe(kek,sizeof kek);
}
static void gestures() {
    otp::PatternGesture g;
    auto add=[&](const char* points){g.clear();for(auto* p=points;*p;++p)g.add(*p-'0');};
    add("123");assert(!g.valid());add("1236");assert(g.valid());
    add("12369");assert(g.valid());add("123698");assert(g.valid());
    add("1236987");assert(g.overflow()&&!g.valid());assert(!strcmp(g.points(),"123698"));
    add("11223366");assert(g.valid()&&!strcmp(g.points(),"1236"));
    add("13");assert(!strcmp(g.points(),"123"));
    add("17");assert(!strcmp(g.points(),"147"));
    add("19");assert(!strcmp(g.points(),"159"));
    add("37");assert(!strcmp(g.points(),"357"));
    add("513");assert(!strcmp(g.points(),"5123"));
    add("219");assert(!strcmp(g.points(),"2159"));
    for(auto invalid:{"","123","1234567","1123","0236","1a36","1356"})assert(!otp::valid_pattern(invalid));
    assert(!otp::valid_pattern(nullptr));assert(otp::valid_pattern("6541"));
    // Fast and slow paths hit exactly the same points, including grazed nodes.
    const int left=otp::PatternGesture::center_x(1),right=otp::PatternGesture::center_x(3);
    const int top=otp::PatternGesture::center_y(1),middle=otp::PatternGesture::center_y(4),bottom=otp::PatternGesture::center_y(7);
    g.start(left,top);g.move(right,top);g.move(right,middle);assert(g.valid()&&!strcmp(g.points(),"1236"));
    g.clear();assert(!g.active()&&!g.valid()&&!g.points()[0]);g.move(left,top);assert(!g.size());
    g.start(left,top);for(int x=left+1;x<=right;++x)g.move(x,top);for(int y=top+1;y<=middle;++y)g.move(right,y);
    assert(g.valid()&&!strcmp(g.points(),"1236"));
    g.clear();g.start(left,top);g.move(left,bottom);g.move(right,bottom);g.move(right,top);assert(g.overflow()&&!g.valid());
    g.clear();assert(!g.overflow());
    puts("PASS: gesture lengths, repeats, midpoints, fast/slow swipes, overflow, cancellation");
}
static void migration() {
    legacy_fixture();otp::Vault v;assert(v.begin());assert(v.credential_kind()==otp::CredentialKind::LegacyPin);
    assert(!v.unlock_pattern("1236"));assert(counter==0);assert(!v.unlock_legacy_pin("87654321"));
    assert(v.unlock_legacy_pin("12345678"));assert(!v.ready()&&v.count()==1);
    assert(!v.create_pattern("1236"));assert(!v.change_pattern("123"));assert(!v.ready());
    assert(v.change_pattern("1236"));assert(v.ready()&&v.count()==1);
    v.lock();assert(!v.unlock_legacy_pin("12345678"));assert(v.unlock_pattern("1236"));
    assert(!strcmp(v.record(0).issuer,"PrivateMail"));
    for(auto& bytes:flash) {LegacyHeader h;memcpy(&h,bytes.data(),sizeof h);assert(h.version==2);}
    v.lock();otp::Vault reboot;assert(reboot.begin());assert(reboot.unlock_pattern("1236"));assert(reboot.count()==1);
    // Every flash boundary during conversion: use the credential selected by
    // the last committed snapshot, never fall back after a wrong credential.
    for(int cut=0;cut<8;++cut) {
        legacy_fixture();otp::Vault m;assert(m.begin());assert(m.unlock_legacy_pin("12345678"));
        cut_after=cut;assert(!m.change_pattern("1236"));cut_after=-1;
        assert(m.status()==otp::VaultStatus::Locked);
        bool legacy=cut<4;assert((m.credential_kind()==otp::CredentialKind::LegacyPin)==legacy);
        assert(legacy?m.unlock_legacy_pin("12345678"):m.unlock_pattern("1236"));assert(m.count()==1);m.lock();
        otp::Vault r;assert(r.begin());assert((r.credential_kind()==otp::CredentialKind::LegacyPin)==legacy);
        assert(legacy?r.unlock_legacy_pin("12345678"):r.unlock_pattern("1236"));assert(r.count()==1);
    }
    puts("PASS: independent v1 fixture, migration, old PIN rejection, both snapshots, eight power-cut boundaries");
}
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
    fresh();otp::Vault v;assert(v.begin());assert(v.status()==otp::VaultStatus::Empty);assert(v.create_pattern("1236"));
    auto r=record();assert(v.upsert(r));
    for(auto& p:flash){std::string memory(reinterpret_cast<char*>(p.data()),p.size());assert(memory.find("PrivateMail")==std::string::npos);assert(memory.find("12345678901234567890")==std::string::npos);}
    v.lock();assert(v.count()==0);assert(!v.unlock_pattern("6541"));assert(v.unlock_pattern("1236"));assert(v.count()==1);
    assert(v.change_pattern("123698"));v.lock();assert(!v.unlock_pattern("1236"));assert(!v.unlock_pattern("123694"));assert(v.unlock_pattern("123698"));
    std::array<otp::Record,100> many;for(int i=0;i<100;i++)many[i]=record(i);
    assert(v.replace(many.data(),many.size()));v.lock();assert(v.unlock_pattern("123698"));assert(v.count()==100);
    auto extra=record(101);assert(!v.upsert(extra));assert(v.remove(many[99].id));assert(v.count()==99);
    assert(v.reset());assert(v.status()==otp::VaultStatus::Empty);
    for(auto& p:flash)for(auto b:p)assert(b==255);
    puts("PASS: encrypted persistence, pattern change, 100 records, deletion, reset");
}
static std::vector<uint8_t> hex(const char* text){std::vector<uint8_t> result;for(size_t i=0;text[i];i+=2){unsigned b=0;sscanf(text+i,"%2x",&b);result.push_back(b);}return result;}
static void crypto_interop(){
    uint8_t salt[16],key[32],zero[32]{},nonce[12]{},plain[16]{},cipher[16]{},tag[16]{};
    for(unsigned i=0;i<16;i++)salt[i]=i;
    assert(otp::derive_key("12345678",salt,key));
    assert(!memcmp(key,hex("1f0be1be8f35627c8cdf3e7f396a01f5dac7da508e5830499d41f9b6d8baeafe").data(),32));
    // Independent Python hashlib/OpenSSL vectors, including the sixth point.
    const char* kdf_vectors[][2]={
        {"StopWatch/Pattern/v1:1236","90a9c87da0d853a51aeae81107a5235d2f3c88f90d5ec19f6d13f3f3bf29c201"},
        {"StopWatch/Pattern/v1:123698","352677ed53a69d98cccda622f92d491380590eedbea78213aa916fbbf78983ea"},
        {"StopWatch/Pattern/v1:123694","85d9a0c36488c37c487d52bb64bd36fd53ea5a94925bc4a0cbc446f94142ccc5"},
        {"","ffa81c4acdf4b1394384005f0aaf083dd4c7bae97d8dc466581ecf75bb005eb9"}
    };
    for (const auto& test : kdf_vectors) {
        assert(otp::derive_key(test[0],salt,key));
        assert(!memcmp(key,hex(test[1]).data(),32));
    }
    // Compare the optimized path with upstream at HMAC key block boundaries
    // and with different binary salts. No timing requirement on the host CPU.
    for (size_t length : {8u,12u,23u,25u,63u,64u,65u,128u}) {
        std::string password(length,' ');
        for (size_t i=0;i<length;++i) password[i]=static_cast<char>(1+(i*37+length)%255);
        uint8_t test_salt[16],reference[32];
        for (size_t i=0;i<16;++i) test_salt[i]=static_cast<uint8_t>(i*length);
        auto start=std::chrono::steady_clock::now();
        assert(mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
            reinterpret_cast<const uint8_t*>(password.data()),password.size(),
            test_salt,16,otp::KdfIterations,32,reference)==0);
        auto baseline=std::chrono::steady_clock::now();
        assert(otp::derive_key(password.c_str(),test_salt,key));
        auto finish=std::chrono::steady_clock::now();
        assert(!memcmp(key,reference,32));
        if (length==25) printf("Host PBKDF2 (600000 iterations): upstream %.1f ms, cached HMAC %.1f ms\n",
            std::chrono::duration<double,std::milli>(baseline-start).count(),
            std::chrono::duration<double,std::milli>(finish-baseline).count());
    }
    assert(otp::encrypt(zero,nonce,nullptr,0,plain,16,cipher,tag));
    assert(!memcmp(cipher,hex("cea7403d4d606b6e074ec5d3baf39d18").data(),16));
    assert(!memcmp(tag,hex("d0d1c8a799996bf0265b98b5d48ab919").data(),16));
    tag[0]^=1;memset(plain,0xff,16);assert(!otp::decrypt(zero,nonce,nullptr,0,cipher,16,tag,plain));
    for(auto b:plain)assert(b==0);
    puts("PASS: PBKDF2/AES-GCM match independent Node/OpenSSL vectors; bad tag wipes output");
}
static void power_cuts(){
    for(int cut=0;cut<4;cut++){
        fresh();{otp::Vault v;assert(v.begin());assert(v.create_pattern("1236"));assert(v.upsert(record()));cut_after=cut;assert(!v.upsert(record(1)));}
        cut_after=-1;otp::Vault reboot;assert(reboot.begin());assert(reboot.unlock_pattern("1236"));assert(reboot.count()==1);assert(!strcmp(reboot.record(0).id,record().id));
    }
    fresh();{otp::Vault v;assert(v.begin());assert(v.create_pattern("1236"));assert(v.upsert(record()));}
    // Corrupt newest ciphertext. CRC rejects it before trying the PIN, older snapshot survives.
    flash[1][150]^=1;
    {otp::Vault v;assert(v.begin());assert(v.unlock_pattern("1236"));assert(v.count()==0);}
    flash[0][0]^=1;{otp::Vault v;assert(v.begin());assert(v.status()==otp::VaultStatus::Damaged);assert(!v.unlock_pattern("1236"));}
    for(int cut=0;cut<8;cut++){
        fresh();otp::Vault v;assert(v.begin());assert(v.create_pattern("1236"));assert(v.upsert(record()));
        cut_after=cut;assert(!v.change_pattern("6541"));cut_after=-1;
        const char* expected=cut<4?"1236":"6541";
        assert(v.unlock_pattern(expected));assert(v.count()==1);v.lock();
        otp::Vault reboot;assert(reboot.begin());assert(reboot.unlock_pattern(expected));assert(reboot.count()==1);
    }
    puts("PASS: cuts during erase/header/payload/commit, snapshot fallback, damaged vault");
}
static void retries(){
    fresh();otp::Vault v;assert(v.begin());assert(v.create_pattern("1236"));v.lock();
    for(int i=0;i<5;i++)assert(!v.unlock_pattern("1478"));assert(v.wait_seconds()==30);assert(!v.unlock_pattern("1236"));
    clock_us+=29000000;otp::Vault reboot;assert(reboot.begin());assert(reboot.wait_seconds()==30);
    clock_us+=31000000;assert(reboot.unlock_pattern("1236"));assert(reboot.wait_seconds()==0);assert(counter==0);
    reboot.lock();commit_fail=true;assert(!reboot.unlock_pattern("1236"));assert(reboot.status()==otp::VaultStatus::StorageError);
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
    fresh();otp::Vault v;assert(v.begin());assert(v.create_pattern("1236"));assert(v.upsert(record()));
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
    assert(v.unlock_pattern("1236"));authorized=true;
    assert(ok(rpc(p,"status")));assert(!ok(rpc(p,"restore_commit")));assert(!confirm_action);
    puts("PASS: USB authorization, physical confirmation, paginated metadata, export, atomic restore, DTR reset, frame recovery");
}
static void migration_usb() {
    legacy_fixture();otp::Vault v;assert(v.begin());assert(v.unlock_legacy_pin("12345678"));
    bool authorized=false;otp::Ui ui(v,authorized);otp::Protocol p(v,ui,authorized);assert(p.begin());
    usb_connected=true;p.tick();authorized=true;
    auto* status=rpc(p,"status");auto* result=cJSON_GetObjectItem(status,"result");
    assert(!strcmp(cJSON_GetObjectItem(result,"state")->valuestring,"locked"));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"authorized")));cJSON_Delete(status);
    for(auto op:{"list","export_begin","time_set","restore_begin"}) {authorized=true;assert(!ok(rpc(p,op)));assert(!confirm_action);}
    assert(v.count()==1&&!v.ready());ui.lock();assert(v.status()==otp::VaultStatus::Locked);
    puts("PASS: pending PIN migration rejects USB metadata, export and mutations even with stale authorization");
}
#ifndef STOPWATCH_UI_TESTS
int main(){setvbuf(stdout,nullptr,_IONBF,0);gestures();vectors();crypto_interop();encryption();power_cuts();retries();migration();protocol_tests();migration_usb();puts("ALL NATIVE TESTS PASSED");}
#endif
