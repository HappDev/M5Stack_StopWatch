#include "ui.h"
#include "board.h"
#include "clock.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstdio>
#include <utility>
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
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
lv_obj_t* Ui::icon_button(bool settings,int x,std::function<void()> action) {
    auto* o=button(settings?LV_SYMBOL_SETTINGS:"",x,209,48,48,std::move(action));
    lv_obj_set_style_radius(o,LV_RADIUS_CIRCLE,0);lv_obj_set_style_pad_all(o,0,0);
    if(settings) lv_obj_set_style_text_font(lv_obj_get_child(o,0),&lv_font_montserrat_28,0);
    else {
        auto* shackle=lv_obj_create(o);lv_obj_set_pos(shackle,17,10);lv_obj_set_size(shackle,14,19);
        lv_obj_set_style_bg_opa(shackle,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(shackle,2,0);
        lv_obj_set_style_border_color(shackle,lv_color_hex(0xe8f4f2),0);lv_obj_set_style_radius(shackle,8,0);
        auto* body=lv_obj_create(o);lv_obj_set_pos(body,13,23);lv_obj_set_size(body,22,15);
        lv_obj_set_style_bg_color(body,lv_color_hex(0xe8f4f2),0);lv_obj_set_style_border_width(body,0,0);lv_obj_set_style_radius(body,3,0);
        for(auto* part:{shackle,body}) {lv_obj_remove_flag(part,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(part,LV_OBJ_FLAG_SCROLLABLE);}
    }
    return o;
}
void Ui::update_clock() {
    if(!clock_label_) return;
    char text[32];format_display_time(board::now(),board::rtc_valid(),text,sizeof text);lv_label_set_text(clock_label_,text);
}
void Ui::update_codes() {
    if(!vault_.ready() || busy_) return;
    const bool valid=board::rtc_valid();const int64_t now=board::now();
    for(size_t i=0;i<vault_.count();++i) if(list_codes_[i]) {
        const auto& r=vault_.record(i);const int64_t step=valid?now/r.period:-2;
        if(list_steps_[i]!=step) {
            char code[9]{};
            if(!valid || !totp(r,now,code)) strcpy(code,r.digits==8?"--------":"------");
            lv_label_set_text(list_codes_[i],code);list_steps_[i]=step;wipe(code,sizeof code);
        }
    }
    if(view_==View::Code && code_label_ && selected_<vault_.count()) {
        const auto& r=vault_.record(selected_);char code[9]{},text[64];
        if(show_tx_ && ble_ && ble_->result()==BleTx::Sending) {
            memcpy(code,sending_code_,sizeof code);strcpy(text,"Отправка…");
        }
        else if(!valid || !totp(r,now,code)) {strcpy(code,"-- -- --");strcpy(text,"Синхронизируйте время");}
        else snprintf(text,sizeof text,"Ещё %u с",static_cast<unsigned>(r.period-now%r.period));
        lv_label_set_text(code_label_,code);lv_label_set_text(seconds_label_,text);wipe(code,sizeof code);
    }
}
void Ui::start_busy(bool saving) {
    busy_=true;saving_=saving;busy_frame_=millis();render();lv_refr_now(nullptr);
}
void Ui::service_busy() {
    if(millis()-busy_frame_<33) return;
    busy_frame_=millis();update_clock();lv_timer_handler();
    #ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(1));
    #endif
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
    if(busy_) return false;
    const bool expired = session_expired(), was_dark = dark_;
    if (expired) lock(dark_);
    last_activity_ = millis();
    if (dark_) { dark_ = false; board::screen(true); render(); }
    return !expired && !was_dark;
}
void Ui::dim() {
    if(ble_) {ble_->cancel();ble_->cancel_pairing();}
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
    pattern_submit_=pin_submit_=pattern_second_=pattern_change_=false;
    view_=View::Recovery; render();
}
void Ui::message(const char* text) { text_ = text; view_ = View::Error; render(); }
void Ui::cancel() {
    auto action = std::move(pending_); pending_ = {}; answer_ = -1;
    if (action) action(false);
}
void Ui::lock(bool dark) {
    if(busy_) return;
    if(ble_) {ble_->cancel();ble_->cancel_pairing();}
    wipe(sending_code_,sizeof sending_code_);show_tx_=false;send_feedback_.clear();
    cancel(); usb_ = false;
    if(on_lock_) on_lock_();
    if(!text_.empty()) wipe(text_.data(),text_.size());
    text_.clear();
    vault_.lock(); unlocked_at_=0; wipe(pin_,sizeof pin_); wipe(first_,sizeof first_);
    gesture_.clear(); wipe(pattern_path_,sizeof pattern_path_); pattern_submit_=pin_submit_=false;
    pin_len_ = 0; pattern_change_ = pattern_second_ = false; view_ = auth_view(); render();
    if (dark) { lv_refr_now(nullptr); dark_ = true; board::screen(false); }
}
bool Ui::ask(const std::string& text, std::function<void(bool)> action) {
    if (pending_ || busy_) return false;
    if(ble_) {ble_->cancel();ble_->cancel_pairing();}
    text_ = text; pending_ = std::move(action); answer_ = -1; view_ = View::Confirm; render(); return true;
}
void Ui::refresh() {
    if (!vault_.ready()) { lock(); return; }
    if (view_ == View::List || view_ == View::Code || view_ == View::Settings || view_==View::Bluetooth) render();
}
void Ui::pin_key(int key) {
    if (busy_ || vault_.wait_seconds()) return;
    if (key == 10) { if (pin_len_) pin_[--pin_len_] = 0; }
    else if (key >= 0 && key <= 9 && pin_len_ < 12) { pin_[pin_len_++] = '0' + key; pin_[pin_len_] = 0; }
    if (pin_label_) { char mask[13]{}; memset(mask,'*',pin_len_); lv_label_set_text(pin_label_,mask); }
}
void Ui::submit_pin() {
    pin_submit_=false;
    if (busy_ || view_!=View::Pin || vault_.wait_seconds() || vault_.credential_kind()!=CredentialKind::LegacyPin || vault_.status()!=VaultStatus::Locked) return;
    if (!Vault::valid_pin(pin_)) { if (info_label_) lv_label_set_text(info_label_,"Введите 8–12 цифр"); return; }
    start_busy(false);
    bool ok = vault_.unlock_legacy_pin(pin_,[this]{service_busy();});
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
    start_busy(enroll);
    KdfService service=[this]{service_busy();};
    bool ok=create ? vault_.create_pattern(pattern,service) : enroll ? vault_.change_pattern(pattern,service) : vault_.unlock_pattern(pattern,service);
    wipe(pattern,sizeof pattern); wipe(first_,sizeof first_); pattern_second_=pattern_change_=false; busy_=false;
    if (ok && vault_.ready()) unlocked_at_=esp_timer_get_time();
    touch();
    if (ok && vault_.ready()) { view_=View::List; render(); }
    else { lock(); if (info_label_) lv_label_set_text(info_label_,vault_.error()); }
}
void Ui::select(size_t index, bool send) {
    if(index>=vault_.count() || !vault_.ready() || dark_ || session_expired()) return;
    if(ble_) {ble_->cancel();ble_->cancel_pairing();}
    wipe(sending_code_,sizeof sending_code_);show_tx_=false;send_feedback_.clear();
    selected_ = index; view_ = View::Code; render();
    if(send) send_code();
}
void Ui::send_code() {
    if(!ble_ || !vault_.ready() || dark_ || busy_ || session_expired() || view_!=View::Code || selected_>=vault_.count()) return;
    if(ble_->result()==BleTx::Sending) return;
    show_tx_=true;send_feedback_.clear();
    char code[9]{};
    if(!board::rtc_valid()) {
        show_tx_=false; // The existing countdown label explains the RTC error.
    } else if(!totp(vault_.record(selected_),board::now(),code)) {
        send_feedback_="Код недоступен";
    } else {
        if(ble_->send(code,esp_timer_get_time())) {
            memcpy(sending_code_,code,sizeof code);
            if(code_label_) lv_label_set_text(code_label_,code);
        }
    }
    wipe(code,sizeof code);update_bluetooth();
}
void Ui::bluetooth_action(int action) {
    if(!ble_ || !vault_.ready() || dark_ || session_expired()) return;
    if(action==0) ble_->enable(!ble_->enabled());
    if(action==1 && ble_->pair(esp_timer_get_time())) view_=View::Pairing;
    if(action==2) {
        ask("Забыть компьютер?\nДля ввода потребуется\nновое сопряжение.",[this](bool yes){
            if(yes) ble_->forget();
            view_=View::Bluetooth;
        });
        return;
    }
    render();
}
void Ui::update_bluetooth() {
    if(!ble_ || !bluetooth_label_) return;
    const char* text=ble_->status();
    if(view_==View::Code && show_tx_) text=send_feedback_.empty()?ble_->tx_status():send_feedback_.c_str();
    lv_label_set_text(bluetooth_label_,text);
}
void Ui::settings_action(int action) {
    if (action == 0 && vault_.ready()) { usb_ = !usb_; render(); }
    if (action == 1 && vault_.ready()) {
        if(ble_) {ble_->cancel();ble_->cancel_pairing();}
        usb_=false; if (on_lock_) on_lock_();
        wipe(first_,sizeof first_); gesture_.clear(); pattern_submit_=false;
        pattern_change_=true; pattern_second_=false; view_=View::Pattern; render();
    }
    if (action == 2) ask(ble_ ? "Удалить ВСЕ аккаунты\nи сопряжение Bluetooth?\nНужна резервная копия.\nОтменить сброс нельзя." :
        "Удалить ВСЕ аккаунты?\nНужна резервная копия.\nОтменить сброс нельзя.", [this](bool yes) {
        if (yes) {
            if(ble_ && !ble_->reset()) {message("Не удалось удалить\nсопряжение Bluetooth.\nПовторите сброс.");return;}
            vault_.reset();lock();
        }
        else if (vault_.ready()) { view_ = View::Settings; render(); }
        else { recovery(); }
    });
}
void Ui::render() {
    if ((view_==View::List || view_==View::Code || view_==View::Settings || view_==View::Bluetooth || view_==View::Pairing) && !vault_.ready()) view_=auth_view();
    retry_visible_=false;
    pattern_panel_=pattern_line_=nullptr; std::fill(std::begin(pattern_dots_),std::end(pattern_dots_),nullptr);
    pin_label_ = code_label_ = seconds_label_ = info_label_ = clock_label_ = nullptr;
    bluetooth_label_=nullptr;
    if(ble_) { rendered_comparison_=ble_->comparison();rendered_number_=ble_->number();rendered_bonded_=ble_->bonded();rendered_tx_=ble_->result(); }
    list_codes_.fill(nullptr);list_steps_.fill(-1);last_second_=UINT32_MAX;
    auto screen = lv_screen_active(); lv_obj_clean(screen);
    rendered_time_valid_=board::rtc_valid(); rendered_usb_=usb_;
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x081615),0); lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    clock_label_=label("",110,23,246);update_clock();
    if(busy_) {
        auto* spinner=lv_spinner_create(screen);lv_obj_set_pos(spinner,197,188);lv_obj_set_size(spinner,72,72);
        lv_spinner_set_anim_params(spinner,900,220);
        lv_obj_set_style_arc_color(spinner,lv_color_hex(0x173c39),LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner,lv_color_hex(0x63e6c2),LV_PART_INDICATOR);
        label(saving_?"Сохранение ключа…":"Проверка ключа…",70,285,326);return;
    }
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
        button("OK",279,274,74,42,[this]{pin_submit_=true;});
        info_label_ = label("8–12 цифр",85,329,296);
        button("Помощь",168,377,130,38,[this]{recovery();});
    } else if (view_ == View::Pattern) {
        bool enroll=pattern_change_ || vault_.status()==VaultStatus::Empty || vault_.credential_kind()==CredentialKind::LegacyPin;
        label(pattern_second_ ? "Повторите рисунок" : enroll ? "Создайте ключ" : "Нарисуйте ключ",75,55,316);
        pattern_panel_=lv_obj_create(screen); lv_obj_set_pos(pattern_panel_,PatternGesture::PanelX,PatternGesture::PanelY);
        lv_obj_set_size(pattern_panel_,PatternGesture::PanelWidth,PatternGesture::PanelHeight);
        lv_obj_set_style_pad_all(pattern_panel_,0,0); lv_obj_set_style_border_width(pattern_panel_,0,0);
        lv_obj_set_style_bg_opa(pattern_panel_,LV_OPA_TRANSP,0);
        lv_obj_remove_flag(pattern_panel_,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(pattern_panel_,LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(pattern_panel_,pattern_event,LV_EVENT_ALL,this);
        pattern_line_=lv_line_create(pattern_panel_); lv_obj_remove_flag(pattern_line_,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(pattern_line_,4,0); lv_obj_set_style_line_rounded(pattern_line_,true,0);
        for (int p=1;p<=9;++p) {
            auto* dot=lv_obj_create(pattern_panel_); pattern_dots_[p-1]=dot;
            lv_obj_set_size(dot,21,21); lv_obj_set_pos(dot,PatternGesture::center_x(p)-10,PatternGesture::center_y(p)-10);
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
        auto panel = lv_obj_create(screen); lv_obj_set_pos(panel,68,105); lv_obj_set_size(panel,330,needs_usb?184:260);
        lv_obj_set_style_bg_opa(panel,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(panel,0,0);
        lv_obj_set_style_pad_all(panel,0,0); lv_obj_set_scroll_dir(panel,LV_DIR_VER);
        for (size_t i = 0; i < vault_.count(); ++i) {
            const auto& r = vault_.record(i);
            auto b = button("",0,static_cast<int>(i*72),314,64,[this,i]{select(i,true);});
            lv_obj_set_parent(b,panel);
            lv_obj_set_style_pad_all(b,0,0);
            auto* name=lv_obj_get_child(b,0);lv_obj_set_align(name,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(name,8,8);lv_obj_set_size(name,144,24);
            lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_LEFT,0);
            lv_label_set_text(name,r.issuer);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
            auto* account=lv_label_create(b);lv_obj_set_pos(account,8,32);lv_obj_set_size(account,144,24);
            lv_obj_set_style_text_font(account,&font_ru,0);lv_label_set_text(account,r.account);lv_label_set_long_mode(account,LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(account,lv_color_hex(0xa2bdb8),0);lv_obj_set_style_text_align(account,LV_TEXT_ALIGN_LEFT,0);
            auto* code=lv_label_create(b);lv_obj_set_pos(code,162,16);lv_obj_set_width(code,144);
            lv_obj_set_style_text_font(code,&lv_font_montserrat_28,0);lv_obj_set_style_text_align(code,LV_TEXT_ALIGN_RIGHT,0);
            list_codes_[i]=code;
            for(auto* child:{name,account,code})lv_obj_remove_flag(child,LV_OBJ_FLAG_CLICKABLE);
        }
        if (!vault_.count()) label("Добавьте аккаунт\nчерез сайт и USB",85,175,296);
        if (needs_usb) button(usb_ ? "Закрыть USB" : "Разрешить USB",98,304,270,48,[this]{settings_action(0);});
        icon_button(true,10,[this]{view_=View::Settings;render();});
        icon_button(false,408,[this]{lock();});
    } else if (view_ == View::Code) {
        if (selected_ >= vault_.count()) { view_ = View::List; render(); return; }
        const auto& r = vault_.record(selected_);
        auto issuer=label(r.issuer,70,85,326);lv_obj_set_height(issuer,28);lv_label_set_long_mode(issuer,LV_LABEL_LONG_DOT);
        auto account=label(r.account,60,115,346);lv_obj_set_height(account,48);lv_label_set_long_mode(account,LV_LABEL_LONG_DOT);
        code_label_ = label("",45,185,376,&lv_font_montserrat_48);
        if(ble_) {
            lv_obj_add_flag(code_label_,LV_OBJ_FLAG_CLICKABLE);
            auto* callback=new std::function<void()>([this]{if(touch())send_code();});
            lv_obj_add_event_cb(code_label_,click,LV_EVENT_CLICKED,callback);
            lv_obj_add_event_cb(code_label_,deleted,LV_EVENT_DELETE,callback);
            bluetooth_label_=label("",60,rendered_time_valid_?305:277,346);
        }
        seconds_label_ = label("",75,265,316);
        if(ble_ && !rendered_time_valid_) lv_obj_set_y(seconds_label_,245);
        if (!rendered_time_valid_) button(usb_ ? "Закрыть USB" : "Разрешить USB",98,310,270,44,[this]{settings_action(0);});
        button("Аккаунты",143,rendered_time_valid_?350:366,180,50,[this]{if(ble_)ble_->cancel();view_=View::List;render();});
        last_second_ = UINT32_MAX;
    } else if (view_ == View::Settings) {
        label("Настройки",90,65,286);
        button(usb_ ? "Закрыть USB" : "Разрешить USB",98,105,270,46,[this]{settings_action(0);});
        button("Сменить ключ",98,159,270,46,[this]{settings_action(1);});
        button("Сбросить всё",98,213,270,46,[this]{settings_action(2);});
        if(ble_) button("Bluetooth",98,267,270,46,[this]{view_=View::Bluetooth;render();});
        char zone[20];format_zone(display_offset_minutes(),zone,sizeof zone);
        label(board::rtc_valid() ? zone : "Синхронизируйте время",75,326,316);
        button("Назад",153,365,160,45,[this]{view_=View::List;render();});
    } else if(view_==View::Bluetooth && ble_) {
        label("Bluetooth",90,65,286);
        bluetooth_label_=label("",65,105,336);
        button(ble_->enabled()?"Выключить":"Включить",98,153,270,46,[this]{bluetooth_action(0);});
        if(ble_->enabled() && !ble_->bonded()) button("Сопряжение",98,211,270,46,[this]{bluetooth_action(1);});
        if(ble_->bonded()) button("Забыть компьютер",98,269,270,46,[this]{bluetooth_action(2);});
        button("Назад",153,365,160,45,[this]{view_=View::Settings;render();});
    } else if(view_==View::Pairing && ble_) {
        label("Сопряжение",85,65,296);
        bluetooth_label_=label("",65,108,336);
        if(ble_->comparison()) {
            char number[7];snprintf(number,sizeof number,"%06u",static_cast<unsigned>(ble_->number()));
            label(number,60,172,346,&lv_font_montserrat_48);
            label("Число совпадает\nс числом на компьютере?",65,235,336);
            button("Совпадает",100,300,266,46,[this]{
                ble_->tick(esp_timer_get_time(),vault_.ready()&&!dark_&&!session_expired());
                ble_->confirm(true);render();
            });
        } else label("Выберите StopWatch OTP\nв настройках Bluetooth ПК.\nОжидание до 60 секунд.",65,170,336);
        button("Отмена",153,365,160,45,[this]{ble_->confirm(false);ble_->cancel_pairing();view_=View::Bluetooth;render();});
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
    update_codes();
    update_bluetooth();
}
void Ui::tick() {
    if(busy_) return;
    if (session_expired()) lock(dark_);
    if(ble_) {
        if(!board::rtc_valid()) ble_->cancel();
        ble_->tick(esp_timer_get_time(),vault_.ready()&&!dark_);
        if(ble_->result()!=BleTx::Sending) wipe(sending_code_,sizeof sending_code_);
        if(rendered_tx_!=ble_->result()) {rendered_tx_=ble_->result();if(view_==View::Code)update_codes();}
        if(view_==View::Pairing && !ble_->pairing()) {view_=View::Bluetooth;last_activity_=millis();render();}
        else if(view_==View::Pairing && (rendered_comparison_!=ble_->comparison() || rendered_number_!=ble_->number())) render();
        else if(view_==View::Bluetooth && rendered_bonded_!=ble_->bonded()) render();
        update_bluetooth();
    }
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
                    if (b) { if(ble_){ble_->cancel();ble_->cancel_pairing();} view_ = View::List; render(); }
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
    if (millis() - last_activity_ >= 30000 && !dark_ && !(ble_&&ble_->pairing())) dim();
    if (pattern_submit_) submit_pattern();
    if (pin_submit_) submit_pin();
    if (!dark_ && (view_==View::List || view_==View::Code || view_==View::Settings) &&
        (rendered_time_valid_!=board::rtc_valid() || rendered_usb_!=usb_)) render();
    uint32_t second = millis()/1000;
    if (second != last_second_ && !dark_) {
        last_second_ = second;
        update_clock();update_codes();
        if ((view_ == View::Pin || view_==View::Pattern) && info_label_) {
            const auto wait=vault_.wait_seconds();
            if(wait) {
                char text[64]; snprintf(text,sizeof text,"Повторите через %lu с",static_cast<unsigned long>(wait)); lv_label_set_text(info_label_,text);
                retry_visible_=true;
            } else if(retry_visible_) {
                lv_label_set_text(info_label_,view_==View::Pin?"Введите 8–12 цифр":"Соедините 4–6 точек");retry_visible_=false;
            }
        }
    }
    if (dark_ && !board::usb_power() && !(ble_&&ble_->active())) {
        board::sleep();
        // A touch IRQ or a side button wakes the display; timer wake keeps it dark.
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) touch();
    }
}
}
