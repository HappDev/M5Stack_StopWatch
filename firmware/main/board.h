#pragma once
#include <cstdint>
#include <ctime>
#include <lvgl.h>
namespace board {
constexpr uint32_t TouchPollPeriodMs = 10;
bool begin();
void tick();
bool rtc_valid();
bool set_time(int64_t epoch);
int64_t now();
uint16_t battery_mv();
bool usb_power();
void screen(bool on);
void sleep();
bool button_a();
bool button_b();
bool button_b_held();
bool wake_touch();
}
