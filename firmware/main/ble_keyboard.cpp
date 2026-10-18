#include "ble_keyboard.h"
#include "model.h"
#include <cstring>

namespace otp {
bool BleKeyboard::begin() {
    if (!port_.load(enabled_)) { error_=true; return false; }
    if (enabled_) { active_=port_.start(); error_=!active_; }
    link_=port_.link(); return !error_;
}
bool BleKeyboard::enable(bool enabled) {
    cancel(); cancel_pairing();
    if (!port_.save_enabled(enabled)) { error_=true; return false; }
    enabled_=enabled; error_=false;
    if (!enabled) { port_.stop(); active_=false; }
    else if (!active_) { active_=port_.start(); error_=!active_; }
    link_=port_.link(); needs_release_=true; return !error_;
}
bool BleKeyboard::pair(int64_t now_us) {
    link_=port_.link();
    if (!active_ || link_.bonded || link_.fault) return false;
    cancel(); pairing_=true; pair_deadline_=now_us+60000000;
    port_.pairing(true); return true;
}
void BleKeyboard::cancel_pairing() {
    if (pairing_) { port_.pairing(false); pairing_=false; }
    link_.comparison=false;
}
void BleKeyboard::confirm(bool yes) {
    if (!comparison()) return;
    port_.confirm(yes); link_.comparison=false;
    if (!yes) cancel_pairing();
}
bool BleKeyboard::forget() {
    cancel(); cancel_pairing();
    // Starting the port is necessary to remove Bluedroid's persisted bonds even
    // when the user has disabled the radio.
    bool temporary=!active_;
    if (temporary && !port_.start()) { error_=true; return false; }
    bool ok=port_.forget();
    if (temporary) port_.stop();
    link_=port_.link(); needs_release_=true; error_=!ok; return ok;
}
bool BleKeyboard::reset() { bool ok=forget(); return enable(false) && ok; }
void BleKeyboard::release() {
    const uint8_t zero[8]{};
    if (active_ && link_.connected && link_.secure && link_.subscribed) {
        needs_release_=!port_.report(zero,sizeof zero);
    } else needs_release_=true;
    down_=false;
}
void BleKeyboard::cancel() {
    if (tx_==BleTx::Sending) { release(); tx_=BleTx::Cancelled; }
    wipe(digits_,sizeof digits_); position_=length_=0;
}
bool BleKeyboard::send(const char* digits, int64_t now_us) {
    if (tx_==BleTx::Sending) return false;
    // Re-read the port at the point of the physical action; the last UI tick
    // may predate a disconnect or a lost notification subscription.
    link_=port_.link();
    if (!ready()) { tx_=BleTx::NotConnected; return false; }
    size_t n=digits ? std::strlen(digits) : 0;
    if (n!=6 && n!=8) { tx_=BleTx::Invalid; return false; }
    for (size_t i=0;i<n;++i) if (digits[i]<'0' || digits[i]>'9') { tx_=BleTx::Invalid; return false; }
    if (needs_release_) { release(); if (needs_release_) { tx_=BleTx::Failed; return false; } }
    std::memcpy(digits_,digits,n); length_=n; position_=0; down_=false;
    next_report_=now_us; tx_=BleTx::Sending; return true;
}
void BleKeyboard::tick(int64_t now_us, bool input_allowed) {
    BleLink current=port_.link();
    bool lost=current.generation!=link_.generation || !current.connected || !current.secure || !current.subscribed || current.fault;
    link_=current;
    if (lost) needs_release_=true;
    if (pairing_ && (!input_allowed || now_us>=pair_deadline_ || link_.fault)) cancel_pairing();
    if (pairing_ && link_.bonded && link_.secure) { port_.pairing(false); pairing_=false; }
    if (tx_==BleTx::Sending && (!input_allowed || lost || !ready())) {
        cancel(); tx_=input_allowed ? BleTx::Failed : BleTx::Cancelled;
    }
    if (ready() && needs_release_) release();
    if (tx_!=BleTx::Sending || now_us<next_report_) return;
    uint8_t report[8]{};
    if (!down_) { char d=digits_[position_]; report[2]=d=='0' ? 0x27 : static_cast<uint8_t>(0x1e + d - '1'); }
    if (!port_.report(report,sizeof report)) { cancel(); tx_=BleTx::Failed; return; }
    if (down_) {
        down_=false;
        if (++position_==length_) { wipe(digits_,sizeof digits_); length_=position_=0; tx_=BleTx::Sent; }
    } else down_=true;
    next_report_=now_us+20000;
}
const char* BleKeyboard::status() const {
    if (error_ || link_.fault) return "Ошибка Bluetooth";
    if (!enabled_) return "Bluetooth выключен";
    if (pairing_) return comparison() ? "Сравните число на ПК" : "Поиск компьютера…";
    return ready() ? "Bluetooth подключён" : "Bluetooth не подключён";
}
const char* BleKeyboard::tx_status() const {
    switch(tx_) {
        case BleTx::Sending: return "Отправка…";
        case BleTx::Sent: return "Код отправлен";
        case BleTx::NotConnected: return "Bluetooth не подключён";
        case BleTx::Invalid: return "Код недоступен";
        case BleTx::Failed: return "Ошибка передачи";
        case BleTx::Cancelled: return "Передача отменена";
        default: return status();
    }
}
}
