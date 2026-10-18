#pragma once
#include "vault.h"
#include "pattern.h"
#include "ble_keyboard.h"
#include <functional>
#include <utility>
#include <string>
#include <lvgl.h>
namespace otp {
class Ui {
public:
    Ui(Vault& vault, bool& usb, BleKeyboard* ble = nullptr) : vault_(vault), usb_(usb), ble_(ble) {}
    void begin();
    void tick();
    void refresh();
    bool ask(const std::string& message, std::function<void(bool)> action);
    void cancel();
    void lock(bool dark = false);
    void message(const char* text);
    void set_lock_hook(std::function<void()> hook) { on_lock_=std::move(hook); }
private:
    enum class View { Pin, Pattern, Recovery, List, Code, Settings, Bluetooth, Pairing, Confirm, Error };
    void render();
    View auth_view() const;
    void pin_key(int key);
    void submit_pin();
    void submit_pattern();
    void start_busy(bool saving);
    void service_busy();
    void update_clock();
    void update_codes();
    void cancel_pattern();
    void draw_pattern();
    static void pattern_event(lv_event_t* event);
    void settings_action(int action);
    void select(size_t index);
    void send_code(size_t index);
    void next_list_page();
    void bluetooth_action(int action);
    void update_bluetooth();
    bool touch();
    bool session_expired() const;
    void dim();
    void recovery();
    lv_obj_t* label(const char* text, int x, int y, int w, const lv_font_t* font = nullptr);
    lv_obj_t* button(const char* text, int x, int y, int w, int h, std::function<void()> action);
    lv_obj_t* icon_button(bool settings, int x, std::function<void()> action);
    static void click(lv_event_t* event);
    static void deleted(lv_event_t* event);
    Vault& vault_; bool& usb_;
    BleKeyboard* ble_ = nullptr;
    lv_obj_t* bluetooth_label_ = nullptr;
    std::string send_feedback_;
    char sending_code_[9]{};
    bool show_tx_ = false, rendered_comparison_ = false, rendered_bonded_ = false;
    BleTx rendered_tx_ = BleTx::Idle;
    uint32_t rendered_number_ = 0;
    uint32_t send_feedback_started_ = 0;
    View view_ = View::Pin;
    bool dark_ = false, pattern_change_ = false, pattern_second_ = false;
    bool busy_ = false, pattern_submit_ = false, pin_submit_ = false, saving_ = false;
    bool retry_visible_ = false;
    bool rendered_time_valid_ = false, rendered_usb_ = false;
    char pin_[13]{}, first_[7]{};
    PatternGesture gesture_;
    lv_obj_t* pattern_panel_ = nullptr;
    lv_obj_t* pattern_dots_[9]{};
    lv_obj_t* pattern_line_ = nullptr;
    lv_point_precise_t pattern_path_[6]{};
    size_t pin_len_ = 0, selected_ = 0;
    lv_obj_t* list_panel_ = nullptr;
    int32_t list_scroll_ = 0;
    uint32_t last_activity_ = 0, last_second_ = 0;
    uint32_t busy_frame_ = 0;
    int64_t unlocked_at_ = 0;
    lv_obj_t* pin_label_ = nullptr;
    lv_obj_t* code_label_ = nullptr;
    lv_obj_t* seconds_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    std::array<lv_obj_t*, MaxRecords> list_codes_{};
    std::array<int64_t, MaxRecords> list_steps_{};
    std::string text_;
    std::function<void(bool)> pending_;
    std::function<void()> on_lock_;
    int answer_ = -1;
};
}
