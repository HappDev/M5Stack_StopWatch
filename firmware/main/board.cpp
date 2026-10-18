/* Hardware initialization adapted from M5Stack M5StopWatch-UserDemo (MIT),
 * Copyright (c) 2026 M5Stack Technology CO LTD. See THIRD_PARTY.md. */
#include "board.h"
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <M5PM1.h>
#include <M5IOE1.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <algorithm>

namespace board {
class Panel : public lgfx::Panel_AMOLED {
public:
    Panel() { _cfg.memory_width = _cfg.panel_width = 480; _cfg.memory_height = _cfg.panel_height = 480;
        _write_depth = _read_depth = lgfx::color_depth_t::rgb565_2Byte; }
    const uint8_t* getInitCommands(uint8_t list) const override {
        static constexpr uint8_t commands[] = {0x11, CMD_INIT_DELAY, 150, 0xC4,1,0x80, 0x35,1,0x80,
            0x44,2,0x01,0xD2, 0x53,1,0x20, 0x20,0, 0x36,1,0, 0x51,1,0x70, 0x29,0, 0xff,0xff};
        return list == 0 ? commands : nullptr;
    }
};
class Display : public M5GFX {
    lgfx::Bus_SPI bus_; Panel panel_;
public:
    bool init_impl(bool reset, bool clear) override {
        auto bus = bus_.config(); bus.freq_write = 80000000; bus.freq_read = 10000000;
        bus.pin_sclk = 40; bus.pin_io0 = 41; bus.pin_io1 = 42; bus.pin_io2 = 46; bus.pin_io3 = 45;
        bus.spi_host = SPI2_HOST; bus.spi_mode = 0; bus.spi_3wire = true; bus.dma_channel = SPI_DMA_CH_AUTO;
        bus_.config(bus); panel_.setBus(&bus_);
        auto panel = panel_.config(); panel.pin_rst = -1; panel.pin_cs = 39;
        panel.panel_width = 468; panel.panel_height = 466; panel.offset_x = 6; panel.offset_y = 0; panel.readable = false;
        panel_.config(panel); setPanel(&panel_); gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT);
        if(!LGFX_Device::init_impl(reset, clear) || !panel_.initPanelFb()) return false;
        // The CO5300 requires even update coordinates. The manufacturer's
        // framebuffer adapter handles alignment and arbitrary LVGL dirty areas.
        auto* framebuffer=panel_.getPanelFb();
        if(!framebuffer) return false;
        framebuffer->setBus(&bus_);framebuffer->setAutoDisplay(true);setPanel(framebuffer);
        return true;
    }
    void brightness(uint8_t v) { panel_.setBrightness(v); }
};
static Display display;
static M5PM1 pm;
static M5IOE1 ioe;
static i2c_master_bus_handle_t bus = nullptr;
static i2c_master_dev_handle_t rtc = nullptr, touch = nullptr;
static bool time_ok = false;
static int64_t last_rtc_check = 0, last_battery_check = 0;
static uint16_t battery = 0; static bool external = false;
static bool a_event = false, b_event = false, held_event = false;
static bool screen_on = true, touch_wake = false, consume_touch = false;
static bool a_down = false, b_down = false, b_long = false;
static int64_t a_since = 0, b_since = 0;
static int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint8_t to_bcd(int n) { return (n / 10) * 16 + n % 10; }
static bool read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t n) {
    return dev && i2c_master_transmit_receive(dev, &reg, 1, data, n, 40) == ESP_OK;
}
static bool write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value}; return dev && i2c_master_transmit(dev, data, 2, 40) == ESP_OK;
}
static bool add_device(uint16_t address, i2c_master_dev_handle_t* device) {
    i2c_device_config_t cfg{}; cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7; cfg.device_address = address; cfg.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &cfg, device) == ESP_OK;
}
static bool read_rtc(int64_t& epoch) {
    uint8_t flags = 0, control = 0, bytes[7]{};
    if (!read(rtc, 0x1D, &flags, 1) || (flags & 2) || !read(rtc, 0x1E, &control, 1) || (control & 0x40)) return false;
    bool stable=false;
    for(unsigned attempt=0;attempt<3;++attempt) {
        uint8_t seconds=0;
        if(!read(rtc,0x10,bytes,7)||!read(rtc,0x10,&seconds,1)) return false;
        if((seconds&0x7f)==(bytes[0]&0x7f)) {stable=true;break;}
    }
    if(!stable) return false;
    // Reject invalid BCD and calendar dates; never normalize or silently repair the RTC.
    const uint8_t masks[] = {0x7f,0x7f,0x3f,0x7f,0x3f,0x1f,0xff};
    for (int i : {0,1,2,4,5,6}) if (((bytes[i] & masks[i]) & 15) > 9 || ((bytes[i] & masks[i]) >> 4) > 9) return false;
    tm t{}; t.tm_sec = bcd(bytes[0] & 0x7f); t.tm_min = bcd(bytes[1] & 0x7f);
    t.tm_hour = bcd(bytes[2] & 0x3f); t.tm_mday = bcd(bytes[4] & 0x3f);
    t.tm_mon = bcd(bytes[5] & 0x1f) - 1; t.tm_year = bcd(bytes[6]) + 100;
    if (t.tm_sec > 59 || t.tm_min > 59 || t.tm_hour > 23 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) return false;
    tm original = t; epoch = static_cast<int64_t>(mktime(&t));
    return epoch >= 946684800 && t.tm_year == original.tm_year && t.tm_mon == original.tm_mon && t.tm_mday == original.tm_mday;
}
static void flush(lv_display_t* d, const lv_area_t* area, uint8_t* pixels) {
    uint32_t width = area->x2 - area->x1 + 1, height = area->y2 - area->y1 + 1;
    display.startWrite(); display.setAddrWindow(area->x1, area->y1, width, height);
    auto src = reinterpret_cast<const lgfx::rgb565_t*>(pixels);
    for (uint32_t off = 0; off < width * height; off += 4096)
        display.writePixels(src + off, std::min<uint32_t>(4096, width * height - off));
    display.endWrite(); lv_display_flush_ready(d);
}
static void touch_read(lv_indev_t*, lv_indev_data_t* data) {
    uint8_t bytes[7]{}; data->state = LV_INDEV_STATE_RELEASED;
    if (!read(touch, 0, bytes, 7)) return;
    int event = bytes[3] >> 6;
    int x = ((bytes[3] & 15) << 8) | bytes[4], y = ((bytes[5] & 15) << 8) | bytes[6];
    if (bytes[2] && (event == 0 || event == 2) && x < 466 && y < 466) {
        if(!screen_on) {touch_wake=true;consume_touch=true;}
        if(consume_touch) return;
        data->point.x = x; data->point.y = y; data->state = LV_INDEV_STATE_PRESSED;
    } else consume_touch=false;
}
bool begin() {
    setenv("TZ", "UTC0", 1); tzset();
    if (i2c_master_get_bus_handle(0, &bus) != ESP_OK) {
        i2c_master_bus_config_t cfg{}; cfg.i2c_port = I2C_NUM_0; cfg.sda_io_num = GPIO_NUM_47; cfg.scl_io_num = GPIO_NUM_48;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT; cfg.glitch_ignore_cnt = 7; cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) return false;
    }
    if (pm.begin(bus) != M5PM1_OK) return false;
    pm.setI2cSleepTime(0); pm.wdtSet(0); pm.ldoSetPowerHold(true); pm.setChargeEnable(true);
    // Retain the manufacturer's conservative 185 mA charging current.
    pm.gpioSet(M5PM1_GPIO_NUM_3, M5PM1_GPIO_MODE_OUTPUT, 1, M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);
    pm.setSingleResetDisable(true); pm.setLedEnLevel(false);
    if (ioe.begin(bus, 0x4F, 100000) != M5IOE1_OK && ioe.begin(bus, 0x6F, 100000) != M5IOE1_OK) return false;
    ioe.setI2cSleepTime(0);
    for (auto pin : {M5IOE1_PIN_1,M5IOE1_PIN_3,M5IOE1_PIN_4,M5IOE1_PIN_5,M5IOE1_PIN_8,M5IOE1_PIN_9,M5IOE1_PIN_10}) ioe.pinMode(pin, OUTPUT);
    ioe.digitalWrite(M5IOE1_PIN_1, 0); ioe.digitalWrite(M5IOE1_PIN_3, 0);
    ioe.digitalWrite(M5IOE1_PIN_9, 0); ioe.digitalWrite(M5IOE1_PIN_10, 0);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_14, 0);
    ioe.digitalWrite(M5IOE1_PIN_8, 1); ioe.digitalWrite(M5IOE1_PIN_5, 0); ioe.digitalWrite(M5IOE1_PIN_4, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    ioe.digitalWrite(M5IOE1_PIN_5, 1); ioe.digitalWrite(M5IOE1_PIN_4, 1); vTaskDelay(pdMS_TO_TICKS(150));
    if (!display.init()) return false;
    display.brightness(120);
    if (!add_device(0x32, &rtc) || !add_device(0x15, &touch)) return false;
    // The IMU shares the RTC rail and may retain the factory firmware's active
    // configuration across an ESP reset. Disable its sensors explicitly.
    // Bosch BMI270 SensorAPI: CHIP_ID=0x24, PWR_CTRL=0x7D, PWR_CONF=0x7C.
    i2c_master_dev_handle_t imu=nullptr;
    if(add_device(0x68,&imu)) {
        uint8_t id=0;
        if(read(imu,0,&id,1) && id==0x24) {write(imu,0x7d,0);write(imu,0x7c,1);}
        i2c_master_bus_rm_device(imu);
    }
    int64_t epoch; time_ok = read_rtc(epoch);
    if (time_ok) { timeval tv{static_cast<time_t>(epoch), 0}; settimeofday(&tv, nullptr); }
    for (auto pin : {GPIO_NUM_1,GPIO_NUM_2,GPIO_NUM_13}) {
        gpio_set_direction(pin, GPIO_MODE_INPUT); gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }
    lv_init(); lv_tick_set_cb([]() -> uint32_t { return esp_timer_get_time() / 1000; });
    auto d = lv_display_create(466,466); lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    constexpr size_t size = 466 * 20 * 2;
    auto buf = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!d || !buf) return false;
    lv_display_set_buffers(d, buf, nullptr, size, LV_DISPLAY_RENDER_MODE_PARTIAL); lv_display_set_flush_cb(d, flush);
    auto input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(input, touch_read);
    // Poll independently of the 33 ms render period so short taps are sampled.
    lv_timer_set_period(lv_indev_get_read_timer(input), TouchPollPeriodMs);
    tick(); return true;
}
void tick() {
    int64_t ms = esp_timer_get_time() / 1000;
    bool a = !gpio_get_level(GPIO_NUM_2), b = !gpio_get_level(GPIO_NUM_1);
    if (a && !a_down) a_since = ms;
    if (!a && a_down && ms - a_since >= 30) a_event = true;
    if (b && !b_down) { b_since = ms; b_long = false; }
    if (b && !b_long && ms - b_since >= 1000) { held_event = true; b_long = true; }
    if (!b && b_down && !b_long && ms - b_since >= 30) b_event = true;
    a_down = a; b_down = b;
    if (ms - last_battery_check >= 2000) {
        uint16_t vin = 0; pm.readVbat(&battery); external = pm.readVin(&vin) == M5PM1_OK && vin > 4000; last_battery_check = ms;
    }
    if (ms - last_rtc_check >= 2000) {
        int64_t epoch; time_ok = read_rtc(epoch); last_rtc_check = ms;
        if (time_ok) { timeval tv{static_cast<time_t>(epoch),0}; settimeofday(&tv,nullptr); }
    }
}
bool rtc_valid() { return time_ok; }
int64_t now() { return time(nullptr); }
bool set_time(int64_t epoch) {
    if (epoch < 946684800 || epoch > 4102444799LL) return false;
    time_t seconds = epoch; tm t{}; gmtime_r(&seconds, &t);
    uint8_t control = 0;
    if (!read(rtc, 0x1e, &control, 1) || !write(rtc, 0x1e, control | 0x40)) return time_ok = false;
    uint8_t data[] = {0x10,to_bcd(t.tm_sec),to_bcd(t.tm_min),to_bcd(t.tm_hour),
        static_cast<uint8_t>(1 << t.tm_wday),to_bcd(t.tm_mday),to_bcd(t.tm_mon + 1),to_bcd(t.tm_year - 100)};
    bool ok = i2c_master_transmit(rtc, data, sizeof data, 40) == ESP_OK;
    ok = write(rtc, 0x1e, control & ~0x40) && ok;
    uint8_t flags = 0; ok = read(rtc,0x1d,&flags,1) && ok;
    ok = write(rtc,0x1d,flags & ~2) && ok;
    int64_t actual = 0; ok = ok && read_rtc(actual) && actual >= epoch && actual <= epoch + 1;
    time_ok = ok;
    if (ok) { timeval tv{static_cast<time_t>(actual),0}; settimeofday(&tv,nullptr); }
    return ok;
}
uint16_t battery_mv() { return battery; }
bool usb_power() { return external; }
void screen(bool on) { screen_on=on;display.brightness(on ? 120 : 0); }
void sleep() {
    // Called while the screen is off and unplugged. Light sleep preserves the
    // unlocked session in RAM; timer wake rechecks power, RTC and its deadline.
    for (auto pin : {GPIO_NUM_1,GPIO_NUM_2,GPIO_NUM_13}) gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup(); esp_sleep_enable_timer_wakeup(2000000);
    esp_light_sleep_start();
}
bool button_a() { bool event = a_event; a_event = false; return event; }
bool button_b() { bool event = b_event; b_event = false; return event; }
bool button_b_held() { bool event = held_event; held_event = false; return event; }
bool wake_touch() {bool event=touch_wake;touch_wake=false;return event;}
}
