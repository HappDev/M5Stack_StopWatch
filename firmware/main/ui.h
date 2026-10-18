#pragma once
#include "vault.h"
#include "pattern.h"
#include <functional>
#include <utility>
#include <string>
#include <lvgl.h>
namespace otp {
class Ui {
public:
    Ui(Vault& vault, bool& usb) : vault_(vault), usb_(usb) {}
    void begin();
    void tick();
    void refresh();
    bool ask(const std::string& message, std::function<void(bool)> action);
    void cancel();
    void lock(bool dark = false);
    void message(const char* text);
    void set_lock_hook(std::function<void()> hook) { on_lock_=std::move(hook); }
private:
    enum class View { Pin, Pattern, Recovery, List, Code, Settings, Confirm, Error };
    void render();
    View auth_view() const;
    void pin_key(int key);
    void submit_pin();
    void submit_pattern();
    void cancel_pattern();
    void draw_pattern();
    static void pattern_event(lv_event_t* event);
    void settings_action(int action);
    void select(size_t index);
    bool touch();
    bool session_expired() const;
    void dim();
    void recovery();
    lv_obj_t* label(const char* text, int x, int y, int w, const lv_font_t* font = nullptr);
    lv_obj_t* button(const char* text, int x, int y, int w, int h, std::function<void()> action);
    static void click(lv_event_t* event);
    static void deleted(lv_event_t* event);
    Vault& vault_; bool& usb_;
    View view_ = View::Pin;
    bool dark_ = false, pattern_change_ = false, pattern_second_ = false;
    bool busy_ = false, pattern_submit_ = false;
    bool retry_visible_ = false;
    bool rendered_time_valid_ = false, rendered_usb_ = false;
    char pin_[13]{}, first_[7]{};
    PatternGesture gesture_;
    lv_obj_t* pattern_panel_ = nullptr;
    lv_obj_t* pattern_dots_[9]{};
    lv_obj_t* pattern_line_ = nullptr;
    lv_point_precise_t pattern_path_[6]{};
    size_t pin_len_ = 0, selected_ = 0;
    uint32_t last_activity_ = 0, last_second_ = 0;
    int64_t unlocked_at_ = 0;
    lv_obj_t* pin_label_ = nullptr;
    lv_obj_t* code_label_ = nullptr;
    lv_obj_t* seconds_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    std::string text_;
    std::function<void(bool)> pending_;
    std::function<void()> on_lock_;
    int answer_ = -1;
};
}
