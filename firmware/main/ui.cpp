#include "ui.h"
#include "board.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstdio>
#include <utility>
extern const lv_font_t font_ru;

namespace otp {
static constexpr int64_t UnlockDurationUs = 12LL * 60 * 60 * 1000000;
static uint32_t millis() { return esp_timer_get_time() / 1000; }
void Ui::click(lv_event_t* e) {
    auto* f = static_cast<std::function<void()>*>(lv_event_get_user_data(e));
    if (f) { auto action=*f; action(); } // Rendering may delete the originating button.
}
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
    auto* callback = new std::function<void()>([this, action=std::move(action)] { if (touch()) action(); });
    lv_obj_add_event_cb(o,click,LV_EVENT_CLICKED,callback); lv_obj_add_event_cb(o,deleted,LV_EVENT_DELETE,callback);
    return o;
}
void Ui::begin() {
    lv_obj_add_event_cb(lv_screen_active(),[](lv_event_t* e) { static_cast<Ui*>(lv_event_get_user_data(e))->touch(); },
                        LV_EVENT_PRESSED,this);
    view_ = auth_view(); last_activity_ = millis(); render();
}
Ui::View Ui::auth_view() const {
    return vault_.credential_kind()==CredentialKind::LegacyPin && vault_.status()==VaultStatus::Locked ? View::Pin : View::Pattern;
}
bool Ui::session_expired() const {
    return vault_.ready() && esp_timer_get_time() - unlocked_at_ >= UnlockDurationUs;
}
bool Ui::touch() {
    const bool expired = session_expired(), was_dark = dark_;
    if (expired) lock(dark_);
    last_activity_ = millis();
    if (dark_) { dark_ = false; board::screen(true); render(); }
    return !expired && !was_dark;
}
void Ui::dim() {
    if (!vault_.ready()) { lock(true); return; }
    cancel(); usb_=false;
    if (on_lock_) on_lock_();
    if (view_==View::Confirm) view_=View::List;
    if (view_==View::Pattern) cancel_pattern();
    render(); lv_refr_now(nullptr); dark_=true; board::screen(false);
}
void Ui::recovery() {
    wipe(pin_,sizeof pin_); pin_len_=0; wipe(first_,sizeof first_);
    gesture_.clear(); wipe(pattern_path_,sizeof pattern_path_);
    pattern_submit_=pattern_second_=pattern_change_=false;
    view_=View::Recovery; render();
}
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
    vault_.lock(); unlocked_at_=0; wipe(pin_,sizeof pin_); wipe(first_,sizeof first_);
    gesture_.clear(); wipe(pattern_path_,sizeof pattern_path_); pattern_submit_=false;
    pin_len_ = 0; pattern_change_ = pattern_second_ = false; view_ = auth_view(); render();
    if (dark) { lv_refr_now(nullptr); dark_ = true; board::screen(false); }
}
bool Ui::ask(const std::string& text, std::function<void(bool)> action) {
    if (pending_) return false;
    text_ = text; pending_ = std::move(action); answer_ = -1; view_ = View::Confirm; render(); return true;
}
void Ui::refresh() {
    if (!vault_.ready()) { lock(); return; }
    if (view_ == View::List || view_ == View::Code || view_ == View::Settings) render();
}
void Ui::pin_key(int key) {
    if (busy_ || vault_.wait_seconds()) return;
    if (key == 10) { if (pin_len_) pin_[--pin_len_] = 0; }
    else if (key >= 0 && key <= 9 && pin_len_ < 12) { pin_[pin_len_++] = '0' + key; pin_[pin_len_] = 0; }
    if (pin_label_) { char mask[13]{}; memset(mask,'*',pin_len_); lv_label_set_text(pin_label_,mask); }
}
void Ui::submit_pin() {
    if (busy_ || vault_.wait_seconds() || vault_.credential_kind()!=CredentialKind::LegacyPin || vault_.status()!=VaultStatus::Locked) return;
    if (!Vault::valid_pin(pin_)) { if (info_label_) lv_label_set_text(info_label_,"Введите 8–12 цифр"); return; }
    busy_=true;
    if (info_label_) lv_label_set_text(info_label_,"Проверка…");
    lv_refr_now(nullptr);
    bool ok = vault_.unlock_legacy_pin(pin_);
    wipe(pin_,sizeof pin_); pin_len_ = 0; busy_=false;
    touch();
    if (ok) { usb_=false; view_ = View::Pattern; render(); }
    else { view_ = View::Pin; render(); if (info_label_) lv_label_set_text(info_label_,vault_.error()); }
}
void Ui::cancel_pattern() {
    wipe(first_,sizeof first_); gesture_.clear(); wipe(pattern_path_,sizeof pattern_path_);
    pattern_second_=pattern_change_=pattern_submit_=false;
    if (!vault_.ready()) lock();
    else { view_=View::List; render(); }
}
void Ui::draw_pattern() {
    if (!pattern_panel_) return;
    for (int p=1;p<=9;++p) lv_obj_set_style_bg_color(pattern_dots_[p-1],
        lv_color_hex(gesture_.contains(p) ? (gesture_.overflow()?0xff8070:0x63e6c2) : 0x62847f),0);
    wipe(pattern_path_,sizeof pattern_path_);
    for (size_t i=0;i<gesture_.size();++i) {
        int p=gesture_.points()[i]-'0';
        pattern_path_[i].x=PatternGesture::center_x(p); pattern_path_[i].y=PatternGesture::center_y(p);
    }
    if (gesture_.size()<2) lv_obj_add_flag(pattern_line_,LV_OBJ_FLAG_HIDDEN);
    else {
        lv_line_set_points(pattern_line_,pattern_path_,gesture_.size());
        lv_obj_set_style_line_color(pattern_line_,lv_color_hex(gesture_.overflow()?0xff8070:0x63e6c2),0);
        lv_obj_remove_flag(pattern_line_,LV_OBJ_FLAG_HIDDEN);
    }
}
void Ui::pattern_event(lv_event_t* event) {
    auto* ui=static_cast<Ui*>(lv_event_get_user_data(event));
    if (ui->view_!=View::Pattern || ui->busy_ || ui->dark_ || ui->pattern_submit_ || ui->vault_.wait_seconds()) return;
    auto code=lv_event_get_code(event);
    if (code==LV_EVENT_PRESS_LOST) { ui->gesture_.clear(); ui->draw_pattern(); return; }
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING && code!=LV_EVENT_RELEASED) return;
    auto* indev=lv_event_get_indev(event); if (!indev) return;
    lv_point_t point; lv_indev_get_point(indev,&point);
    lv_area_t area; lv_obj_get_coords(ui->pattern_panel_,&area);
    int x=point.x-area.x1,y=point.y-area.y1;
    if (!ui->touch()) return;
    if (code==LV_EVENT_PRESSED) ui->gesture_.start(x,y);
    else if (ui->gesture_.active()) ui->gesture_.move(x,y);
    ui->draw_pattern();
    // Defer KDF and screen deletion until LVGL has finished dispatching events.
    if (code==LV_EVENT_RELEASED && ui->gesture_.active()) ui->pattern_submit_=true;
}
void Ui::submit_pattern() {
    pattern_submit_=false;
    if (busy_ || view_!=View::Pattern || vault_.wait_seconds()) { gesture_.clear(); return; }
    if (!gesture_.valid()) {
        gesture_.clear(); draw_pattern();
        if (info_label_) lv_label_set_text(info_label_,"Соедините 4–6 точек");
        return;
    }
    char pattern[7]{}; memcpy(pattern,gesture_.points(),sizeof pattern); gesture_.clear();
    bool create=vault_.status()==VaultStatus::Empty;
    bool enroll=create || pattern_change_ || vault_.credential_kind()==CredentialKind::LegacyPin;
    if (enroll && !pattern_second_) {
        memcpy(first_,pattern,sizeof first_); wipe(pattern,sizeof pattern); pattern_second_=true; render(); return;
    }
    if (enroll && strcmp(first_,pattern)) {
        wipe(first_,sizeof first_); wipe(pattern,sizeof pattern); pattern_second_=false;
        render(); if (info_label_) lv_label_set_text(info_label_,"Рисунки не совпали"); return;
    }
    busy_=true; draw_pattern();
    if (info_label_) lv_label_set_text(info_label_,"Проверка…");
    lv_refr_now(nullptr);
    bool ok=create ? vault_.create_pattern(pattern) : enroll ? vault_.change_pattern(pattern) : vault_.unlock_pattern(pattern);
    wipe(pattern,sizeof pattern); wipe(first_,sizeof first_); pattern_second_=pattern_change_=false; busy_=false;
    if (ok && vault_.ready()) unlocked_at_=esp_timer_get_time();
    touch();
    if (ok && vault_.ready()) { view_=View::List; render(); }
    else { lock(); if (info_label_) lv_label_set_text(info_label_,vault_.error()); }
}
void Ui::select(size_t index) { selected_ = index; view_ = View::Code; render(); }
void Ui::settings_action(int action) {
    if (action == 0 && vault_.ready()) { usb_ = !usb_; render(); }
    if (action == 1 && vault_.ready()) {
        usb_=false; if (on_lock_) on_lock_();
        wipe(first_,sizeof first_); gesture_.clear(); pattern_submit_=false;
        pattern_change_=true; pattern_second_=false; view_=View::Pattern; render();
    }
    if (action == 2) ask("Удалить ВСЕ аккаунты?\nНужна резервная копия.\nОтменить сброс нельзя.", [this](bool yes) {
        if (yes) { vault_.reset(); lock(); }
        else if (vault_.ready()) { view_ = View::Settings; render(); }
        else { recovery(); }
    });
}
void Ui::render() {
    if ((view_==View::List || view_==View::Code || view_==View::Settings) && !vault_.ready()) view_=auth_view();
    retry_visible_=false;
    pattern_panel_=pattern_line_=nullptr; std::fill(std::begin(pattern_dots_),std::end(pattern_dots_),nullptr);
    pin_label_ = code_label_ = seconds_label_ = info_label_ = nullptr;
    auto screen = lv_screen_active(); lv_obj_clean(screen);
    rendered_time_valid_=board::rtc_valid(); rendered_usb_=usb_;
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
        label("Прежний PIN",100,58,266); pin_label_ = label("",100,88,266,&lv_font_montserrat_28);
        char mask[13]{}; memset(mask,'*',pin_len_); lv_label_set_text(pin_label_,mask);
        for (int i = 0; i < 9; ++i) button(std::to_string(i+1).c_str(),113+(i%3)*83,130+(i/3)*48,74,42,[this,i]{pin_key(i+1);});
        button("<",113,274,74,42,[this]{pin_key(10);}); button("0",196,274,74,42,[this]{pin_key(0);});
        button("OK",279,274,74,42,[this]{submit_pin();});
        info_label_ = label("8–12 цифр",85,329,296);
        button("Помощь",168,377,130,38,[this]{recovery();});
    } else if (view_ == View::Pattern) {
        bool enroll=pattern_change_ || vault_.status()==VaultStatus::Empty || vault_.credential_kind()==CredentialKind::LegacyPin;
        label(pattern_second_ ? "Повторите рисунок" : enroll ? "Создайте ключ" : "Нарисуйте ключ",75,55,316);
        pattern_panel_=lv_obj_create(screen); lv_obj_set_pos(pattern_panel_,63,88); lv_obj_set_size(pattern_panel_,340,320);
        lv_obj_set_style_pad_all(pattern_panel_,0,0); lv_obj_set_style_border_width(pattern_panel_,0,0);
        lv_obj_set_style_bg_opa(pattern_panel_,LV_OPA_TRANSP,0);
        lv_obj_remove_flag(pattern_panel_,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(pattern_panel_,LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(pattern_panel_,pattern_event,LV_EVENT_ALL,this);
        pattern_line_=lv_line_create(pattern_panel_); lv_obj_remove_flag(pattern_line_,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(pattern_line_,5,0); lv_obj_set_style_line_rounded(pattern_line_,true,0);
        for (int p=1;p<=9;++p) {
            auto* dot=lv_obj_create(pattern_panel_); pattern_dots_[p-1]=dot;
            lv_obj_set_size(dot,26,26); lv_obj_set_pos(dot,PatternGesture::center_x(p)-13,PatternGesture::center_y(p)-13);
            lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_border_width(dot,0,0);
            lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(dot,LV_OBJ_FLAG_SCROLLABLE);
        }
        info_label_=label("Соедините 4–6 точек",70,405,326); draw_pattern();
        label(enroll && vault_.status()!=VaultStatus::Empty ? "Синяя: отмена" : "Синяя: меню",143,433,180);
    } else if (view_ == View::Recovery) {
        label("Помощь",90,80,286);
        label("Забыли ключ?\nСброс удалит все аккаунты.\nДля восстановления\nнужна резервная копия.",70,135,326);
        button("Сбросить всё",98,270,270,48,[this]{settings_action(2);});
        button("Назад",153,340,160,48,[this]{view_=auth_view();render();});
    } else if (view_ == View::List) {
        const bool needs_usb=!rendered_time_valid_ || !vault_.count();
        label("Аккаунты",100,65,266);
        auto panel = lv_obj_create(screen); lv_obj_set_pos(panel,67,105); lv_obj_set_size(panel,332,needs_usb?184:248);
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
        if (needs_usb) button(usb_ ? "Закрыть USB" : "Разрешить USB",98,304,270,48,[this]{settings_action(0);});
        button("Настройки",101,370,160,45,[this]{view_=View::Settings;render();});
        button("Замок",273,370,92,45,[this]{lock();});
    } else if (view_ == View::Code) {
        if (selected_ >= vault_.count()) { view_ = View::List; render(); return; }
        const auto& r = vault_.record(selected_);
        auto issuer=label(r.issuer,70,85,326);lv_obj_set_height(issuer,28);lv_label_set_long_mode(issuer,LV_LABEL_LONG_DOT);
        auto account=label(r.account,60,115,346);lv_obj_set_height(account,48);lv_label_set_long_mode(account,LV_LABEL_LONG_DOT);
        code_label_ = label("",45,185,376,&lv_font_montserrat_48);
        seconds_label_ = label("",75,265,316);
        if (!rendered_time_valid_) button(usb_ ? "Закрыть USB" : "Разрешить USB",98,310,270,44,[this]{settings_action(0);});
        button("Аккаунты",143,rendered_time_valid_?350:366,180,50,[this]{view_=View::List;render();});
        last_second_ = UINT32_MAX;
    } else if (view_ == View::Settings) {
        label("Настройки",90,65,286);
        button(usb_ ? "Закрыть USB" : "Разрешить USB",98,115,270,48,[this]{settings_action(0);});
        button("Сменить ключ",98,178,270,48,[this]{settings_action(1);});
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
        label(text_.c_str(),70,140,326); button("Назад",153,330,160,50,[this]{view_=vault_.ready()?View::List:auth_view();render();});
    }
}
void Ui::tick() {
    if (session_expired()) lock(dark_);
    if(board::wake_touch()) touch();
    bool a = board::button_a(), b = board::button_b(), held = board::button_b_held();
    if (a || b || held) {
        bool was_dark = dark_; touch();
        if (!was_dark) {
            if (held) lock();
            else if (b && !pending_ && (view_==View::Pin || view_==View::Pattern || view_==View::Recovery)) {
                if (view_==View::Recovery) { view_=auth_view(); render(); }
                else if (view_==View::Pattern && vault_.status()==VaultStatus::Unlocked) cancel_pattern();
                else recovery();
            }
            else if (vault_.ready() && !pending_) {
                if (view_==View::Pattern) { if (b) cancel_pattern(); }
                else {
                    if (a && vault_.count()) select((selected_ + 1) % vault_.count());
                    if (b) { view_ = View::List; render(); }
                }
            }
        }
    }
    // Touches anywhere, including scroll gestures, count as physical activity.
    auto indev = lv_indev_active();
    (void)indev;
    if (lv_display_get_inactive_time(nullptr) < 100) last_activity_ = millis();
    if (answer_ >= 0 && pending_) {
        bool yes = answer_ == 1; auto action = std::move(pending_); pending_ = {}; answer_ = -1;
        view_ = vault_.ready() ? View::List : auth_view();
        action(yes); render();
    }
    if (millis() - last_activity_ >= 30000 && !dark_) dim();
    if (pattern_submit_) submit_pattern();
    if (!dark_ && (view_==View::List || view_==View::Code || view_==View::Settings) &&
        (rendered_time_valid_!=board::rtc_valid() || rendered_usb_!=usb_)) render();
    uint32_t second = millis()/1000;
    if (second != last_second_ && !dark_) {
        last_second_ = second;
        if ((view_ == View::Pin || view_==View::Pattern) && info_label_) {
            const auto wait=vault_.wait_seconds();
            if(wait) {
                char text[64]; snprintf(text,sizeof text,"Повторите через %lu с",static_cast<unsigned long>(wait)); lv_label_set_text(info_label_,text);
                retry_visible_=true;
            } else if(retry_visible_) {
                lv_label_set_text(info_label_,view_==View::Pin?"Введите 8–12 цифр":"Соедините 4–6 точек");retry_visible_=false;
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
