#pragma once
#include <cstddef>
#include <cstdint>

namespace otp {
// Display preference only. RTC, system TZ and TOTP always remain UTC.
void clock_begin();
bool display_offset_known();
int display_offset_minutes();
bool set_display_offset(int minutes);
void format_zone(int minutes, char* out, size_t capacity);
void format_display_time(int64_t epoch, bool valid, char* out, size_t capacity);
}
