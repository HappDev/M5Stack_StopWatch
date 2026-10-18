#include "clock.h"
#include <nvs.h>
#include <cstdio>
#include <ctime>

namespace otp {
static nvs_handle_t preferences = 0;
static int offset = 0;
static bool known = false;
void clock_begin() {
    if (preferences) nvs_close(preferences);
    preferences=0;offset=0;known=false;
    if(nvs_open("display",NVS_READWRITE,&preferences)!=ESP_OK) {preferences=0;return;}
    uint32_t value=0;
    if(nvs_get_u32(preferences,"utc_offset",&value)==ESP_OK && value<=1560) {
        offset=static_cast<int>(value)-720;known=true;
    }
}
bool display_offset_known() {return known;}
int display_offset_minutes() {return offset;}
bool set_display_offset(int minutes) {
    if(minutes < -720 || minutes > 840 || !preferences) return false;
    if(nvs_set_u32(preferences,"utc_offset",static_cast<uint32_t>(minutes+720))!=ESP_OK || nvs_commit(preferences)!=ESP_OK) return false;
    offset=minutes;known=true;return true;
}
void format_zone(int minutes,char* out,size_t capacity) {
    unsigned absolute=static_cast<unsigned>(minutes<0?-minutes:minutes);
    snprintf(out,capacity,"UTC%c%02u:%02u",minutes<0?'-':'+',absolute/60,absolute%60);
}
void format_display_time(int64_t epoch,bool valid,char* out,size_t capacity) {
    if(!valid) {snprintf(out,capacity,"--:--:--");return;}
    time_t seconds=static_cast<time_t>(epoch+static_cast<int64_t>(offset)*60);tm local{};
    #ifdef _WIN32
    gmtime_s(&local,&seconds);
    #else
    gmtime_r(&seconds,&local);
    #endif
    snprintf(out,capacity,"%02d:%02d:%02d%s",local.tm_hour,local.tm_min,local.tm_sec,known?"":" UTC");
}
}
