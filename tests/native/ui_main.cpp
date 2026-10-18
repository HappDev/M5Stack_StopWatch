// Use the same simulated Flash/NVS fixtures as the storage suite, but link the
// real Ui and real LVGL instead of the confirmation-only UI stub.
#define STOPWATCH_UI_TESTS
#include "main.cpp"
#include <fstream>
#include <cstdlib>

static bool side_a=false,side_b=false,side_held=false,wake=false,lit=true;
static bool plugged=true;
static unsigned sleep_calls=0;
namespace board {
bool usb_power(){return plugged;}
void screen(bool on){lit=on;}
void sleep(){++sleep_calls;}
bool button_a(){bool v=side_a;side_a=false;return v;}
bool button_b(){bool v=side_b;side_b=false;return v;}
bool button_b_held(){bool v=side_held;side_held=false;return v;}
bool wake_touch(){bool v=wake;wake=false;return v;}
}
static lv_indev_t* pointer;
static lv_point_t position{};
static bool pressed=false;
static std::vector<uint8_t> image_pixels(466*466*3);
static lv_obj_t* find_text(const char* text,lv_obj_t* parent=nullptr) {
    if (!parent) parent=lv_screen_active();
    if (lv_obj_check_type(parent,&lv_label_class) && !strcmp(lv_label_get_text(parent),text)) return parent;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i) if(auto* found=find_text(text,lv_obj_get_child(parent,i)))return found;
    return nullptr;
}
static void advance(unsigned ms) {clock_us+=static_cast<int64_t>(ms)*1000;lv_tick_inc(ms);}
static void sample(otp::Ui& ui,int x,int y,bool down) {
    position={x,y};pressed=down;advance(20);lv_obj_update_layout(lv_screen_active());lv_indev_read(pointer);ui.tick();
}
static void click_text(otp::Ui& ui,const char* text) {
    if(!strcmp(text,"Настройки"))text=LV_SYMBOL_SETTINGS;
    auto* label=find_text(text);assert(label);auto* button=lv_obj_get_parent(label);
    assert(lv_obj_check_type(button,&lv_button_class));
    lv_obj_update_layout(lv_screen_active());lv_area_t area;lv_obj_get_coords(button,&area);
    int x=(area.x1+area.x2)/2,y=(area.y1+area.y2)/2;sample(ui,x,y,true);sample(ui,x,y,false);
}
static void draw(otp::Ui& ui,const char* points,bool release=true,int dx=0,int dy=0) {
    for(const char* p=points;*p;++p) sample(ui,otp::PatternGesture::PanelX+otp::PatternGesture::center_x(*p-'0')+dx,otp::PatternGesture::PanelY+otp::PatternGesture::center_y(*p-'0')+dy,true);
    if(release)sample(ui,position.x,position.y,false);
}
static void screenshot(const char* name) {
    const char* folder=std::getenv("STOPWATCH_UI_SCREENSHOTS");if(!folder)return;
    lv_refr_now(nullptr);
    std::ofstream out(std::string(folder)+"/"+name+".ppm",std::ios::binary);
    assert(out);out<<"P6\n466 466\n255\n";out.write(reinterpret_cast<const char*>(image_pixels.data()),image_pixels.size());
}
static void setup_lvgl() {
    lv_init();auto* display=lv_display_create(466,466);
    static uint32_t buffer[466*48];
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,[](lv_display_t* d,const lv_area_t* area,uint8_t* pixels){
        for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x){
            size_t dst=(y*466+x)*3,src=((y-area->y1)*(area->x2-area->x1+1)+(x-area->x1))*4;
            image_pixels[dst]=pixels[src+2];image_pixels[dst+1]=pixels[src+1];image_pixels[dst+2]=pixels[src];
        }
        lv_display_flush_ready(d);
    });
    pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer,LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(pointer,[](lv_indev_t*,lv_indev_data_t* data){data->point=position;data->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;});
}
static void ui_enrollment() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    assert(find_text("Создайте ключ"));screenshot("pattern-create");
    draw(ui,"1236");assert(find_text("Повторите рисунок"));
    draw(ui,"1478");assert(find_text("Рисунки не совпали"));assert(v.status()==otp::VaultStatus::Empty);
    draw(ui,"1236");draw(ui,"1236");assert(v.ready()&&find_text("Аккаунты"));
    assert(v.upsert(record()));ui.refresh();
    click_text(ui,"Настройки");click_text(ui,"Сменить ключ");
    draw(ui,"1258");side_b=true;ui.tick();assert(v.ready()&&find_text("Аккаунты"));
    ui.lock();draw(ui,"1236");click_text(ui,"Настройки");click_text(ui,"Сменить ключ");
    draw(ui,"1258");draw(ui,"1258");assert(v.ready());ui.lock();
    assert(!v.unlock_pattern("1236"));draw(ui,"1258");assert(v.ready());
    ui.lock();draw(ui,"1236987");assert(v.status()==otp::VaultStatus::Locked&&counter==0);
    draw(ui,"123");assert(counter==0);
    draw(ui,"1258",false);assert(v.status()==otp::VaultStatus::Locked);screenshot("pattern-drawing");
    // Simulate loss of capture while still pressed: release must not authenticate.
    auto* panel=lv_obj_get_child(lv_screen_active(),2); // status, title, input panel
    assert(!lv_obj_check_type(panel,&lv_label_class));
    lv_obj_send_event(panel,LV_EVENT_PRESS_LOST,pointer);sample(ui,position.x,position.y,false);
    assert(v.status()==otp::VaultStatus::Locked&&counter==0);
    for(int i=0;i<5;++i)draw(ui,"1236");assert(v.wait_seconds()==30);
    draw(ui,"1258");assert(v.status()==otp::VaultStatus::Locked&&counter==5);
    advance(31000);draw(ui,"1258");assert(v.ready()&&counter==0);
    // Release outside the input panel keeps the captured gesture intact.
    ui.lock();draw(ui,"1258",false);sample(ui,233,425,true);sample(ui,233,425,false);assert(v.ready());
    // Physical side-button wake only turns on the screen, then requires a new gesture.
    ui.lock(true);assert(!lit);side_a=true;ui.tick();assert(lit&&v.status()==otp::VaultStatus::Locked);
    ui.lock(true);wake=true;ui.tick();assert(lit&&v.status()==otp::VaultStatus::Locked);
    draw(ui,"1258");assert(v.ready());ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL enrollment, confirmation mismatch, cancel/change, release, loss, overflow, retries and wake");
}
static void ui_wide_pattern_targets() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    // Start below the bottom row, outside the old panel, and enroll off-center.
    draw(ui,"7896",true,0,40);assert(find_text("Повторите рисунок"));
    draw(ui,"7896",true,0,40);assert(v.ready());
    const int offsets[][2]={{-40,0},{40,0},{0,-40},{0,40}};
    for(const auto& offset:offsets) {
        ui.lock();draw(ui,"7896",true,offset[0],offset[1]);assert(v.ready()&&counter==0);
    }
    ui.lock();draw(ui,"7896");assert(v.ready());ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL off-center pattern enrollment/unlock at all four touch-target boundaries");
}
static void ui_migration() {
    legacy_fixture();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    assert(find_text("Прежний PIN"));
    for(char c:std::string("12345678")){char digit[]={c,0};click_text(ui,digit);}
    // A side-button transition after OK's event but before the main tick must
    // cancel the deferred check instead of authenticating from recovery.
    lv_obj_send_event(lv_obj_get_parent(find_text("OK")),LV_EVENT_CLICKED,nullptr);
    side_b=true;ui.tick();assert(v.status()==otp::VaultStatus::Locked&&counter==0&&find_text("Помощь"));
    click_text(ui,"Назад");
    auto enter_pin=[&]{for(char c:std::string("12345678")){char digit[]={c,0};click_text(ui,digit);}click_text(ui,"OK");};
    enter_pin();assert(v.status()==otp::VaultStatus::Unlocked&&!v.ready());assert(find_text("Создайте ключ"));
    side_a=true;ui.tick();assert(!find_text("Аккаунты")&&!find_text("Настройки")&&!usb);
    draw(ui,"1236");side_b=true;ui.tick();assert(v.status()==otp::VaultStatus::Locked&&find_text("Прежний PIN"));
    enter_pin();draw(ui,"1236");draw(ui,"1236");assert(v.ready()&&v.count()==1&&find_text("Аккаунты"));
    ui.lock();assert(find_text("Нарисуйте ключ")&&!find_text("Прежний PIN"));
    draw(ui,"1236");assert(v.ready()&&!strcmp(v.record(0).issuer,"PrivateMail"));ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL legacy PIN migration, side-button bypass rejection, cancellation and account preservation");
}
static void ui_session() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    draw(ui,"1236");draw(ui,"1236");assert(v.ready());
    const int64_t unlocked=clock_us;
    click_text(ui,"Разрешить USB");assert(usb);
    bool cancelled=false;assert(ui.ask("Test confirmation",[&](bool yes){assert(!yes);cancelled=true;}));
    advance(30000);ui.tick();assert(!lit&&v.ready()&&!usb&&cancelled);
    side_a=true;ui.tick();assert(lit&&v.ready()&&find_text("Аккаунты"));
    advance(30000);ui.tick();assert(!lit&&v.ready());wake=true;ui.tick();assert(lit&&v.ready());
    // Physical activity and RTC changes must not move the 12-hour deadline.
    const int64_t deadline=unlocked+12LL*60*60*1000000;
    advance(static_cast<unsigned>((deadline-clock_us)/1000)-1);side_b=true;ui.tick();assert(v.ready());
    rtc_ok=false;advance(1);ui.tick();assert(!v.ready()&&!usb&&find_text("Нарисуйте ключ"));
    draw(ui,"1236");assert(v.ready());
    advance(30000);ui.tick();assert(!lit&&v.ready());
    advance(12*60*60*1000);ui.tick();assert(!lit&&!v.ready());wake=true;ui.tick();assert(lit&&find_text("Нарисуйте ключ"));
    draw(ui,"1236");side_held=true;ui.tick();assert(!v.ready());
    // Restart never inherits the RAM-only session.
    draw(ui,"1236");assert(v.ready());otp::Vault reboot;assert(reboot.begin()&&!reboot.ready());ui.lock();
    // Deadline also survives the 32-bit millisecond counter wrapping.
    clock_us=(static_cast<int64_t>(UINT32_MAX)-1000)*1000;
    draw(ui,"1236");assert(v.ready());advance(2000);ui.tick();assert(v.ready());
    advance(12*60*60*1000);ui.tick();assert(!v.ready());
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: 12-hour deadline, display sleep/wake, USB revocation, pending cancellation, manual lock, restart and timer wrap");
}
static void ui_recovery_and_usb() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    draw(ui,"1236");draw(ui,"1236");assert(find_text("Разрешить USB"));screenshot("empty-usb");
    rtc_ok=false;ui.tick();assert(find_text("Разрешить USB"));
    assert(v.upsert(record()));ui.refresh();assert(find_text("Разрешить USB"));screenshot("missing-time-usb");
    click_text(ui,"Разрешить USB");assert(usb&&find_text("Закрыть USB"));
    click_text(ui,"Закрыть USB");assert(!usb);
    side_a=true;ui.tick();assert(find_text("Разрешить USB")&&find_text("Синхронизируйте время"));screenshot("code-missing-time-usb");
    rtc_ok=true;ui.tick();assert(!find_text("Разрешить USB"));
    side_b=true;ui.tick();assert(!find_text("Разрешить USB"));
    click_text(ui,"Настройки");assert(find_text("Разрешить USB"));
    click_text(ui,"Разрешить USB");assert(usb);usb=false;ui.tick();assert(find_text("Разрешить USB"));
    ui.lock();assert(!find_text("Сброс")&&!find_text("Сбросить всё"));
    side_b=true;ui.tick();assert(find_text("Помощь")&&find_text("Сбросить всё"));screenshot("recovery");
    click_text(ui,"Сбросить всё");click_text(ui,"Отмена");assert(v.count()==0&&!v.ready()&&find_text("Помощь"));
    click_text(ui,"Назад");draw(ui,"1236");assert(v.ready()&&v.count()==1);
    ui.lock();side_b=true;ui.tick();click_text(ui,"Сбросить всё");click_text(ui,"Разрешить");
    assert(v.status()==otp::VaultStatus::Empty&&find_text("Создайте ключ"));
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: separate reset screen, cancellation preserves accounts, USB shortcuts for missing time/records and live state refresh");
}
static void ui_batch_import() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    draw(ui,"1236");draw(ui,"1236");assert(v.upsert(record()));ui.refresh();
    otp::Protocol p(v,ui,usb);assert(p.begin());usb_connected=true;p.tick();
    click_text(ui,"Настройки");click_text(ui,"Разрешить USB");assert(usb);
    assert(ok(rpc(p,"import_begin","{\"count\":2}")));
    assert(ok(rpc(p,"import_item",item_args(0,record(1)))));assert(ok(rpc(p,"import_item",item_args(1,record(2)))));
    assert(v.count()==1&&!find_text("Подтверждение"));assert(!rpc(p,"import_commit"));
    assert(find_text("Импортировать аккаунты?\nДобавить: 2\nЗаменить: 0\nВсего после: 3\nОдно подтверждение\nдля всего набора."));
    screenshot("batch-import-confirmation");click_text(ui,"Разрешить");
    assert(ok(cJSON_Parse(usb_tx.c_str()))&&v.count()==3&&!find_text("Подтверждение"));
    assert(ok(rpc(p,"import_begin","{\"count\":1}")));assert(ok(rpc(p,"import_item",item_args(0,record(3)))));
    assert(!rpc(p,"import_commit"));click_text(ui,"Отмена");assert(v.count()==3&&!find_text("Подтверждение"));
    assert(ok(rpc(p,"import_begin","{\"count\":1}")));assert(ok(rpc(p,"import_item",item_args(0,record(3)))));
    assert(!rpc(p,"import_commit"));advance(31000);ui.tick();assert(!usb&&v.count()==3&&v.ready());
    assert(!ok(rpc(p,"import_commit")));ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL batch summary, single confirmation, cancellation and display sleep discard staged import");
}
static lv_obj_t* find_type(const lv_obj_class_t* type,lv_obj_t* parent=nullptr) {
    if(!parent)parent=lv_screen_active();
    if(lv_obj_check_type(parent,type))return parent;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i)if(auto* found=find_type(type,lv_obj_get_child(parent,i)))return found;
    return nullptr;
}
static void ui_spinner() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    struct Probe {otp::Ui* ui;unsigned frames=0;int first=-1,last=-1;} probe{&ui};
    auto* timer=lv_timer_create([](lv_timer_t* timer){
        auto& probe=*static_cast<Probe*>(lv_timer_get_user_data(timer));
        if(auto* spinner=find_type(&lv_spinner_class)) {
            int angle=lv_arc_get_angle_start(spinner)*361+lv_arc_get_angle_end(spinner);
            if(probe.first<0){probe.first=angle;screenshot("key-spinner");}probe.last=angle;++probe.frames;
            // No callback may re-enter auth, mutate the vault or leave the busy screen.
            probe.ui->lock();probe.ui->tick();assert(!probe.ui->ask("Blocked",[](bool){assert(false);}));
            assert(find_type(&lv_spinner_class));
        }
    },1,&probe);
    clock_read_step_us=1000;lv_tick_set_cb([]()->uint32_t{return static_cast<uint32_t>(clock_us/1000);});
    draw(ui,"1236");draw(ui,"1236");assert(v.ready());
    ui.lock();draw(ui,"1236");assert(v.ready());
    assert(probe.frames>=4&&probe.first!=probe.last&&!find_type(&lv_spinner_class));
    lv_timer_delete(timer);lv_tick_set_cb(nullptr);clock_read_step_us=0;ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real KDF animates LVGL spinner across frames, with busy input and re-entry blocked");
}
static void ui_account_codes() {
    fresh();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    screenshot("pattern-smaller");draw(ui,"1236");draw(ui,"1236");
    std::array<otp::Record,otp::MaxRecords> rows{};
    for(size_t i=0;i<rows.size();++i){rows[i]=record(i);snprintf(rows[i].issuer,sizeof rows[i].issuer,"Service %u",static_cast<unsigned>(i));}
    strcpy(rows[0].issuer,"Очень длинный сервис для проверки");strcpy(rows[0].account,"long-account-name@example.com");
    rows[1].digits=8;rows[1].period=60;rows[1].algorithm=otp::Algorithm::Sha256;
    assert(v.replace(rows.data(),rows.size()));assert(otp::set_display_offset(480));ui.refresh();
    assert(find_text("07:31:30")&&!find_text("3900 мВ  OTP"));
    char first[9],second[9];assert(otp::totp(rows[0],board_epoch,first)&&otp::totp(rows[1],board_epoch,second));
    auto* first_label=find_text(first);auto* second_label=find_text(second);assert(first_label&&second_label&&strlen(second)==8);
    auto* row=lv_obj_get_parent(first_label);auto* panel=lv_obj_get_parent(row);lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_x(first_label)>lv_obj_get_x(lv_obj_get_child(row,0)));
    lv_point_t text_size;lv_text_get_size(&text_size,second,&lv_font_montserrat_28,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(lv_obj_get_width(first_label)>=text_size.x);
    screenshot("account-list-codes");
    lv_obj_scroll_to_y(panel,216,LV_ANIM_OFF);lv_obj_update_layout(lv_screen_active());int scrolled=lv_obj_get_scroll_y(panel);assert(scrolled>0);
    auto* code_object=first_label;
    board_epoch=1234567920;advance(1000);ui.tick();
    assert(otp::totp(rows[0],board_epoch,first)&&otp::totp(rows[1],board_epoch,second));
    assert(find_text(first)==code_object&&find_text(second)==second_label&&lv_obj_get_scroll_y(panel)==scrolled);
    assert(find_text("07:32:00"));
    rtc_ok=false;ui.tick();assert(find_text("--:--:--")&&find_text("------")&&find_text("--------")&&!find_text(first));
    assert(find_text("Разрешить USB"));screenshot("account-list-time-invalid");
    rtc_ok=true;ui.tick();advance(30000);ui.tick();assert(!lit);
    board_epoch+=60;wake=true;ui.tick();assert(lit);assert(otp::totp(rows[0],board_epoch,first)&&find_text(first));
    click_text(ui,"Настройки");assert(find_text("UTC+08:00"));click_text(ui,"Назад");
    sample(ui,432,233,true);sample(ui,432,233,false);assert(!v.ready()&&find_text("Нарисуйте ключ"));
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: 100 account rows, 6/8 digits, period rollover without scroll reset, local clock, invalid RTC, wake and round controls");
}
static void ui_bluetooth() {
    fresh();FakeBlePort port;port.saved=true;port.connect();otp::BleKeyboard keyboard(port);assert(keyboard.begin());
    otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb,&keyboard);ui.begin();
    draw(ui,"1236");draw(ui,"1236");
    for(int i=0;i<10;++i) {auto r=record(i);snprintf(r.issuer,sizeof r.issuer,"BLE Service %d",i);assert(v.upsert(r));}
    ui.refresh();port.reports.clear();
    char code[9];assert(otp::totp(v.record(0),board_epoch,code));
    auto tap_label=[&](const char* text){
        auto* object=find_text(text);assert(object);lv_obj_update_layout(lv_screen_active());
        lv_area_t area;lv_obj_get_coords(object,&area);
        sample(ui,(area.x1+area.x2)/2,(area.y1+area.y2)/2,true);
        sample(ui,(area.x1+area.x2)/2,(area.y1+area.y2)/2,false);
    };
    auto finish=[&]{for(int i=0;i<20;++i){advance(20);ui.tick();}};
    tap_label("BLE Service 0");assert(keyboard.result()==otp::BleTx::Sending);screenshot("bluetooth-code-sending");
    tap_label(code);finish();assert(port.typed()==code);assert(find_text("Код отправлен"));
    screenshot("bluetooth-code-sent");
    size_t count=port.typed().size();ui.refresh();advance(1000);ui.tick();assert(port.typed().size()==count);
    tap_label(code);finish();assert(port.typed()==std::string(code)+code);
    port.reports.clear();side_a=true;ui.tick();finish();assert(port.typed().empty()&&find_text("BLE Service 1"));
    assert(otp::totp(v.record(1),board_epoch,code));
    // The current code is recomputed at the physical action, even across a TOTP boundary.
    board_epoch+=30;assert(otp::totp(v.record(1),board_epoch,code));
    sample(ui,233,210,true);sample(ui,233,210,false);finish();assert(port.typed()==code);
    port.reports.clear();plugged=false;sleep_calls=0;advance(30000);ui.tick();
    assert(!lit&&keyboard.active()&&port.running&&sleep_calls==0);
    wake=true;ui.tick();finish();assert(lit&&port.typed().empty());
    // An explicit click with no connection is never replayed after reconnecting.
    port.disconnect();ui.tick();tap_label(code);assert(find_text("Bluetooth не подключён"));
    port.connect();finish();assert(port.typed().empty());
    tap_label(code);ui.lock();finish();assert(keyboard.result()!=otp::BleTx::Sending&&!v.ready());
    draw(ui,"1236");port.reports.clear();
    // A scroll gesture must not count as opening a card.
    sample(ui,230,285,true);sample(ui,230,145,true);sample(ui,230,120,false);
    finish();assert(port.typed().empty()&&find_text("Аккаунты"));
    click_text(ui,"Настройки");click_text(ui,"Bluetooth");screenshot("bluetooth-settings-paired");
    click_text(ui,"Забыть компьютер");click_text(ui,"Отмена");assert(keyboard.bonded());
    click_text(ui,"Забыть компьютер");click_text(ui,"Разрешить");assert(!keyboard.bonded());
    click_text(ui,"Сопряжение");assert(port.pair_open);screenshot("bluetooth-pairing");
    advance(31000);ui.tick();assert(lit&&keyboard.pairing());
    port.state.connected=true;port.state.comparison=true;port.state.number=123;
    ui.tick();assert(find_text("000123")&&!port.approved);screenshot("bluetooth-number-confirmation");
    click_text(ui,"Совпадает");assert(port.approved);
    port.connect();ui.tick();assert(!keyboard.pairing()&&find_text("Забыть компьютер"));
    click_text(ui,"Забыть компьютер");click_text(ui,"Разрешить");click_text(ui,"Сопряжение");
    advance(60000);ui.tick();assert(!keyboard.pairing()&&!port.pair_open&&find_text("Bluetooth"));
    click_text(ui,"Сопряжение");side_b=true;ui.tick();assert(!keyboard.pairing()&&find_text("Аккаунты"));
    click_text(ui,"Настройки");click_text(ui,"Bluetooth");click_text(ui,"Выключить");
    assert(!keyboard.active()&&!port.running&&!port.saved);screenshot("bluetooth-settings-off");
    advance(30000);ui.tick();assert(!lit&&sleep_calls>0);wake=true;ui.tick();plugged=true;
    click_text(ui,"Назад");click_text(ui,"Назад");
    rtc_ok=false;ui.tick();tap_label("BLE Service 0");assert(find_text("Синхронизируйте время")&&port.typed().empty());
    screenshot("bluetooth-time-invalid");
    rtc_ok=true;ui.tick();side_b=true;ui.tick();
    click_text(ui,"Настройки");click_text(ui,"Bluetooth");click_text(ui,"Включить");port.connect();ui.tick();
    click_text(ui,"Назад");click_text(ui,"Сбросить всё");port.fail=true;click_text(ui,"Разрешить");
    assert(v.ready()&&v.count()==10&&find_text("Не удалось удалить\nсопряжение Bluetooth.\nПовторите сброс."));
    port.fail=false;click_text(ui,"Назад");click_text(ui,"Настройки");click_text(ui,"Сбросить всё");click_text(ui,"Разрешить");
    assert(v.status()==otp::VaultStatus::Empty&&!keyboard.enabled()&&!keyboard.bonded()&&!port.saved);
    ui.lock();while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: LVGL BLE list-open/repeat, yellow navigation, rollover, scroll, wake, offline, lock, pairing confirmation/timeout, forget and off");
}
int main(){setvbuf(stdout,nullptr,_IONBF,0);radio_entropy_test();setup_lvgl();ui_enrollment();ui_wide_pattern_targets();ui_migration();ui_session();ui_recovery_and_usb();ui_batch_import();ui_spinner();ui_account_codes();ui_bluetooth();puts("ALL LVGL UI TESTS PASSED");}
