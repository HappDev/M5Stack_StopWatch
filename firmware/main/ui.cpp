#include "ui.h"
#include "board.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstdio>
#include <utility>
extern const lv_font_t font_ru;

namespace otp {
static uint32_t millis() { return esp_timer_get_time() / 1000; }
void Ui::click(lv_event_t* e) { auto* f = static_cast<std::function<void()>*>(lv_event_get_user_data(e)); if (f) (*f)(); }
void Ui::deleted(lv_event_t* e) { delete static_cast<std::function<void()>*>(lv_event_get_user_data(e)); }
lv_obj_t* Ui::label(const char* text, int x, int y, int w, const lv_font_t* font) {
    auto* o = lv_label_create(lv_screen_active()); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o, font ? font : &font_ru, 0);
    lv_obj_set_style_text_color(o,lv_color_hex(0xe8f4f2),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0); lv_label_set_text(o,text); return o;
}
lv_obj_t* Ui::button(const char* text, int x, int y, int w, int h, std::function<void()> action) {
    auto* o = lv_button_create(lv_screen_active()); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x173c39),0); lv_obj_set_style_radius(o,14,0);
    auto* l = lv_label_create(o); lv_label_set_text(l,text); lv_obj_set_style_text_font(l,&font_ru,0); lv_obj_center(l);
    auto* callback = new std::function<void()>([this, action=std::move(action)] { touch(); action(); });
    lv_obj_add_event_cb(o,click,LV_EVENT_CLICKED,callback); lv_obj_add_event_cb(o,deleted,LV_EVENT_DELETE,callback);
    return o;
}
void Ui::begin() {
    lv_obj_add_event_cb(lv_screen_active(),[](lv_event_t* e) { static_cast<Ui*>(lv_event_get_user_data(e))->touch(); },
                        LV_EVENT_PRESSED,this);
    view_ = View::Pin; last_activity_ = millis(); render();
}
void Ui::touch() { last_activity_ = millis(); if (dark_) { dark_ = false; board::screen(true); render(); } }
void Ui::message(const char* text) { text_ = text; view_ = View::Error; render(); }
void Ui::cancel() {
    auto action = std::move(pending_); pending_ = {}; answer_ = -1;
    if (action) action(false);
}
void Ui::lock(bool dark) {
    cancel(); usb_ = false;
    if(on_lock_) on_lock_();
    if(!text_.empty()) wipe(text_.data(),text_.size());
    text_.clear();
    vault_.lock(); wipe(pin_,sizeof pin_); wipe(first_,sizeof first_);
    pin_len_ = 0; pin_change_ = pin_second_ = false; view_ = View::Pin; render();
    if (dark) { lv_refr_now(nullptr); dark_ = true; board::screen(false); }
}
bool Ui::ask(const std::string& text, std::function<void(bool)> action) {
    if (pending_) return false;
    text_ = text; pending_ = std::move(action); answer_ = -1; view_ = View::Confirm; render(); return true;
}
void Ui::refresh() {
    if (vault_.status() != VaultStatus::Unlocked) { lock(); return; }
    if (view_ == View::List || view_ == View::Code || view_ == View::Settings) render();
}
void Ui::pin_key(int key) {
    if (key == 10) { if (pin_len_) pin_[--pin_len_] = 0; }
    else if (key >= 0 && key <= 9 && pin_len_ < 12) { pin_[pin_len_++] = '0' + key; pin_[pin_len_] = 0; }
    if (pin_label_) { char mask[13]{}; memset(mask,'*',pin_len_); lv_label_set_text(pin_label_,mask); }
}
void Ui::submit_pin() {
    if (!Vault::valid_pin(pin_)) { if (info_label_) lv_label_set_text(info_label_,"Введите 8–12 цифр"); return; }
    bool create = vault_.status() == VaultStatus::Empty;
    if ((create || pin_change_) && !pin_second_) {
        memcpy(first_,pin_,sizeof first_); wipe(pin_,sizeof pin_); pin_len_ = 0; pin_second_ = true; render(); return;
    }
    if ((create || pin_change_) && strcmp(first_,pin_)) {
        wipe(first_,sizeof first_); wipe(pin_,sizeof pin_); pin_len_ = 0; pin_second_ = false;
        render(); lv_label_set_text(info_label_,"PIN не совпали"); return;
    }
    if (info_label_) lv_label_set_text(info_label_,"Проверка…");
    lv_refr_now(nullptr);
    bool ok = create ? vault_.create(pin_) : pin_change_ ? vault_.change_pin(pin_) : vault_.unlock(pin_);
    wipe(pin_,sizeof pin_); wipe(first_,sizeof first_); pin_len_ = 0; pin_second_ = pin_change_ = false;
    touch();
    if (ok) { view_ = View::List; render(); }
    else { view_ = View::Pin; render(); if (info_label_) lv_label_set_text(info_label_,vault_.error()); }
}
void Ui::select(size_t index) { selected_ = index; view_ = View::Code; render(); }
void Ui::settings_action(int action) {
    if (action == 0) { usb_ = !usb_; render(); }
    if (action == 1) { usb_ = false; pin_change_ = true; pin_second_ = false; pin_len_ = 0; view_ = View::Pin; render(); }
    if (action == 2) ask("Удалить ВСЕ аккаунты?\nНужна резервная копия.\nОтменить сброс нельзя.", [this](bool yes) {
        if (yes) { vault_.reset(); lock(); } else { view_ = View::Settings; render(); }
    });
}
void Ui::render() {
    retry_visible_=false;
    pin_label_ = code_label_ = seconds_label_ = info_label_ = nullptr;
    auto screen = lv_screen_active(); lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x081615),0); lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    char status[64]; snprintf(status,sizeof status,"%u мВ  %s",board::battery_mv(),usb_ ? "USB открыт" : "OTP");
    label(status,110,23,246);
    if (vault_.status() == VaultStatus::Damaged || vault_.status() == VaultStatus::StorageError) {
        if (view_ != View::Confirm) {
            label("Хранилище недоступно",70,100,326); label("Восстановите резервную копию\nпосле сброса устройства.",70,160,326);
            button("Сбросить",143,285,180,50,[this]{ settings_action(2); }); return;
        }
    }
    if (view_ == View::Pin) {
        const char* title = pin_second_ ? "Повторите PIN" : pin_change_ ? "Новый PIN" :
            vault_.status() == VaultStatus::Empty ? "Создайте PIN" : "Введите PIN";
        label(title,100,58,266); pin_label_ = label("",100,88,266,&lv_font_montserrat_28);
        char mask[13]{}; memset(mask,'*',pin_len_); lv_label_set_text(pin_label_,mask);
        for (int i = 0; i < 9; ++i) button(std::to_string(i+1).c_str(),113+(i%3)*83,130+(i/3)*48,74,42,[this,i]{pin_key(i+1);});
        button("<",113,274,74,42,[this]{pin_key(10);}); button("0",196,274,74,42,[this]{pin_key(0);});
        button("OK",279,274,74,42,[this]{submit_pin();});
        info_label_ = label("8–12 цифр",85,329,296);
        if (!pin_change_) button("Сброс",168,377,130,38,[this]{settings_action(2);});
        else button("Отмена",168,377,130,38,[this]{wipe(pin_,sizeof pin_);wipe(first_,sizeof first_);pin_len_=0;pin_change_=pin_second_=false;view_=View::List;render();});
    } else if (view_ == View::List) {
        label("Аккаунты",100,65,266);
        auto panel = lv_obj_create(screen); lv_obj_set_pos(panel,67,105); lv_obj_set_size(panel,332,248);
        lv_obj_set_style_bg_opa(panel,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(panel,0,0);
        lv_obj_set_style_pad_all(panel,0,0); lv_obj_set_scroll_dir(panel,LV_DIR_VER);
        for (size_t i = 0; i < vault_.count(); ++i) {
            const auto& r = vault_.record(i); std::string name = std::string(r.issuer) + "\n" + r.account;
            auto b = button(name.c_str(),0,static_cast<int>(i*72),312,64,[this,i]{select(i);});
            lv_obj_set_parent(b,panel);
            auto l = lv_obj_get_child(b,0); lv_obj_set_width(l,285); lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
            lv_obj_set_height(l,48);
        }
        if (!vault_.count()) label("Добавьте аккаунт\nчерез сайт и USB",85,175,296);
        button("Настройки",101,370,160,45,[this]{view_=View::Settings;render();});
        button("Замок",273,370,92,45,[this]{lock();});
    } else if (view_ == View::Code) {
        if (selected_ >= vault_.count()) { view_ = View::List; render(); return; }
        const auto& r = vault_.record(selected_);
        auto issuer=label(r.issuer,70,85,326);lv_obj_set_height(issuer,28);lv_label_set_long_mode(issuer,LV_LABEL_LONG_DOT);
        auto account=label(r.account,60,115,346);lv_obj_set_height(account,48);lv_label_set_long_mode(account,LV_LABEL_LONG_DOT);
        code_label_ = label("",45,185,376,&lv_font_montserrat_48);
        seconds_label_ = label("",75,265,316);
        button("Аккаунты",143,350,180,50,[this]{view_=View::List;render();});
        last_second_ = 0;
    } else if (view_ == View::Settings) {
        label("Настройки",90,65,286);
        button(usb_ ? "Закрыть USB" : "Разрешить USB",98,115,270,48,[this]{settings_action(0);});
        button("Сменить PIN",98,178,270,48,[this]{settings_action(1);});
        button("Сбросить всё",98,241,270,48,[this]{settings_action(2);});
        label(board::rtc_valid() ? "Время UTC установлено" : "Синхронизируйте время",75,308,316);
        button("Назад",153,365,160,45,[this]{view_=View::List;render();});
    } else if (view_ == View::Confirm) {
        label("Подтверждение",75,70,316);
        auto panel=lv_obj_create(screen);lv_obj_set_pos(panel,65,120);lv_obj_set_size(panel,336,190);
        lv_obj_set_style_bg_opa(panel,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(panel,0,0);
        lv_obj_set_scroll_dir(panel,LV_DIR_VER);lv_obj_set_style_pad_all(panel,5,0);
        auto l=label(text_.c_str(),0,0,310);lv_obj_set_parent(l,panel);lv_obj_set_height(l,LV_SIZE_CONTENT);
        button("Отмена",87,335,140,55,[this]{answer_=0;}); button("Разрешить",239,335,140,55,[this]{answer_=1;});
    } else {
        label(text_.c_str(),70,140,326); button("Назад",153,330,160,50,[this]{view_=vault_.status()==VaultStatus::Unlocked?View::List:View::Pin;render();});
    }
}
void Ui::tick() {
    if(board::wake_touch()) touch();
    bool a = board::button_a(), b = board::button_b(), held = board::button_b_held();
    if (a || b || held) {
        bool was_dark = dark_; touch();
        if (!was_dark) {
            if (held) lock();
            else if (vault_.status() == VaultStatus::Unlocked && !pending_) {
                if (a && vault_.count()) select((selected_ + 1) % vault_.count());
                if (b) { wipe(pin_,sizeof pin_);wipe(first_,sizeof first_);pin_len_=0;pin_change_=pin_second_=false;view_ = View::List; render(); }
            }
        }
    }
    // Touches anywhere, including scroll gestures, count as physical activity.
    auto indev = lv_indev_active();
    (void)indev;
    if (lv_display_get_inactive_time(nullptr) < 100) last_activity_ = millis();
    if (answer_ >= 0 && pending_) {
        bool yes = answer_ == 1; auto action = std::move(pending_); pending_ = {}; answer_ = -1;
        view_ = vault_.status() == VaultStatus::Unlocked ? View::List : View::Pin;
        action(yes); render();
    }
    if (millis() - last_activity_ >= 30000 && !dark_) lock(true);
    uint32_t second = millis()/1000;
    if (second != last_second_ && !dark_) {
        last_second_ = second;
        if (view_ == View::Pin && info_label_) {
            const auto wait=vault_.wait_seconds();
            if(wait) {
                char text[64]; snprintf(text,sizeof text,"Повторите через %lu с",static_cast<unsigned long>(wait)); lv_label_set_text(info_label_,text);
                retry_visible_=true;
            } else if(retry_visible_) {
                lv_label_set_text(info_label_,"Введите 8–12 цифр");retry_visible_=false;
            }
        }
        if (view_ == View::Code && code_label_) {
            const auto& r = vault_.record(selected_); char code[9]{};
            if (!board::rtc_valid() || !totp(r,board::now(),code)) {
                lv_label_set_text(code_label_,"-- -- --"); lv_label_set_text(seconds_label_,"Синхронизируйте время");
            } else {
                lv_label_set_text(code_label_,code); char text[32];
                snprintf(text,sizeof text,"Ещё %u с",static_cast<unsigned>(r.period - board::now()%r.period)); lv_label_set_text(seconds_label_,text);
            }
            wipe(code,sizeof code);
        }
    }
    if (dark_ && !board::usb_power()) {
        board::sleep();
        // A touch IRQ or a side button wakes the display; timer wake keeps it dark.
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) touch();
    }
}
}
