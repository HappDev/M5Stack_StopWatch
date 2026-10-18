// Use the same simulated Flash/NVS fixtures as the storage suite, but link the
// real Ui and real LVGL instead of the confirmation-only UI stub.
#define STOPWATCH_UI_TESTS
#include "main.cpp"
#include <fstream>
#include <cstdlib>

static bool side_a=false,side_b=false,side_held=false,wake=false,lit=true;
namespace board {
bool usb_power(){return true;}
void screen(bool on){lit=on;}
void sleep(){}
bool button_a(){bool v=side_a;side_a=false;return v;}
bool button_b(){bool v=side_b;side_b=false;return v;}
bool button_b_held(){bool v=side_held;side_held=false;return v;}
bool wake_touch(){bool v=wake;wake=false;return v;}
}
static lv_indev_t* pointer;
static lv_point_t position{};
static bool pressed=false;
static std::vector<uint8_t> image_pixels(468*466*3);
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
    auto* label=find_text(text);assert(label);auto* button=lv_obj_get_parent(label);
    assert(lv_obj_check_type(button,&lv_button_class));
    lv_obj_update_layout(lv_screen_active());lv_area_t area;lv_obj_get_coords(button,&area);
    int x=(area.x1+area.x2)/2,y=(area.y1+area.y2)/2;sample(ui,x,y,true);sample(ui,x,y,false);
}
static void draw(otp::Ui& ui,const char* points,bool release=true) {
    for(const char* p=points;*p;++p) sample(ui,103+otp::PatternGesture::center_x(*p-'0'),104+otp::PatternGesture::center_y(*p-'0'),true);
    if(release)sample(ui,position.x,position.y,false);
}
static void screenshot(const char* name) {
    const char* folder=std::getenv("STOPWATCH_UI_SCREENSHOTS");if(!folder)return;
    lv_refr_now(nullptr);
    std::ofstream out(std::string(folder)+"/"+name+".ppm",std::ios::binary);
    assert(out);out<<"P6\n468 466\n255\n";out.write(reinterpret_cast<const char*>(image_pixels.data()),image_pixels.size());
}
static void setup_lvgl() {
    lv_init();auto* display=lv_display_create(468,466);
    static uint32_t buffer[468*48];
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,[](lv_display_t* d,const lv_area_t* area,uint8_t* pixels){
        for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x){
            size_t dst=(y*468+x)*3,src=((y-area->y1)*(area->x2-area->x1+1)+(x-area->x1))*4;
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
    draw(ui,"1258");click_text(ui,"Отмена");assert(v.ready()&&find_text("Аккаунты"));
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
    ui.lock();draw(ui,"1258",false);sample(ui,233,380,true);sample(ui,233,380,false);assert(v.ready());
    // Physical side-button wake only turns on the screen, then requires a new gesture.
    ui.lock(true);assert(!lit);side_a=true;ui.tick();assert(lit&&v.status()==otp::VaultStatus::Locked);
    ui.lock(true);wake=true;ui.tick();assert(lit&&v.status()==otp::VaultStatus::Locked);
    draw(ui,"1258");assert(v.ready());ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL enrollment, confirmation mismatch, cancel/change, release, loss, overflow, retries and wake");
}
static void ui_migration() {
    legacy_fixture();otp::Vault v;assert(v.begin());bool usb=false;otp::Ui ui(v,usb);ui.begin();
    assert(find_text("Прежний PIN"));
    auto enter_pin=[&]{for(char c:std::string("12345678")){char digit[]={c,0};click_text(ui,digit);}click_text(ui,"OK");};
    enter_pin();assert(v.status()==otp::VaultStatus::Unlocked&&!v.ready());assert(find_text("Создайте ключ"));
    side_a=true;ui.tick();side_b=true;ui.tick();assert(!find_text("Аккаунты")&&!find_text("Настройки")&&!usb);
    draw(ui,"1236");click_text(ui,"Отмена");assert(v.status()==otp::VaultStatus::Locked&&find_text("Прежний PIN"));
    enter_pin();draw(ui,"1236");draw(ui,"1236");assert(v.ready()&&v.count()==1&&find_text("Аккаунты"));
    ui.lock();assert(find_text("Нарисуйте ключ")&&!find_text("Прежний PIN"));
    draw(ui,"1236");assert(v.ready()&&!strcmp(v.record(0).issuer,"PrivateMail"));ui.lock();
    while(lv_obj_get_event_count(lv_screen_active()))lv_obj_remove_event(lv_screen_active(),0);
    puts("PASS: real LVGL legacy PIN migration, side-button bypass rejection, cancellation and account preservation");
}
int main(){setvbuf(stdout,nullptr,_IONBF,0);setup_lvgl();ui_enrollment();ui_migration();puts("ALL LVGL UI TESTS PASSED");}
