#pragma once
#include "vault.h"
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
    enum class View { Pin, List, Code, Settings, Confirm, Error };
    void render();
    void pin_key(int key);
    void submit_pin();
    void settings_action(int action);
    void select(size_t index);
    void touch();
    lv_obj_t* label(const char* text, int x, int y, int w, const lv_font_t* font = nullptr);
    lv_obj_t* button(const char* text, int x, int y, int w, int h, std::function<void()> action);
    static void click(lv_event_t* event);
    static void deleted(lv_event_t* event);
    Vault& vault_; bool& usb_;
    View view_ = View::Pin;
    bool dark_ = false, pin_change_ = false, pin_second_ = false;
    bool retry_visible_ = false;
    char pin_[13]{}, first_[13]{};
    size_t pin_len_ = 0, selected_ = 0;
    uint32_t last_activity_ = 0, last_second_ = 0;
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
