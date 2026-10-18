#pragma once
#include <cstddef>
#include <cstdint>

namespace otp {
// All controller/UI methods run on the main task. The ESP port snapshots events
// from the Bluetooth task and never accesses LVGL or the vault.
struct BleLink {
    bool connected=false, secure=false, subscribed=false, bonded=false;
    bool comparison=false, fault=false;
    uint32_t number=0, generation=0;
};
// Shared, testable authorization policy. A Bluetooth callback must enforce the
// deadline itself even when the main task is occupied with a vault operation.
struct BlePairingGate {
    int64_t deadline=0;
    bool approved=false;
    void open(int64_t now) { deadline=now+60000000; approved=false; }
    void close() { deadline=0; approved=false; }
    bool live(int64_t now) const { return deadline!=0 && now<deadline; }
    bool compare(int64_t now, bool bonded) const { return live(now) && !bonded; }
    bool authenticate(int64_t now, bool bonded, bool success, bool protected_link, int bonds) const {
        return success && protected_link && bonds==1 && (bonded || (live(now) && approved));
    }
};
class BlePort {
public:
    virtual ~BlePort() = default;
    virtual bool load(bool& enabled) = 0;
    virtual bool save_enabled(bool enabled) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual BleLink link() = 0;
    virtual void pairing(bool allow) = 0;
    virtual void confirm(bool yes) = 0;
    virtual bool forget() = 0;
    virtual bool report(const uint8_t* data, size_t length) = 0;
};
enum class BleTx { Idle, Sending, Sent, NotConnected, Invalid, Failed, Cancelled };
class BleKeyboard {
public:
    explicit BleKeyboard(BlePort& port) : port_(port) {}
    bool begin();
    void tick(int64_t now_us, bool input_allowed);
    bool enable(bool enabled);
    bool pair(int64_t now_us);
    void cancel_pairing();
    void confirm(bool yes);
    bool forget();
    bool reset();
    bool send(const char* digits, int64_t now_us);
    void cancel();
    bool enabled() const { return enabled_; }
    bool active() const { return active_; }
    bool pairing() const { return pairing_; }
    bool bonded() const { return link_.bonded; }
    bool comparison() const { return pairing_ && link_.comparison; }
    uint32_t number() const { return link_.number; }
    bool ready() const { return active_ && !pairing_ && link_.connected && link_.secure && link_.subscribed && !link_.fault; }
    BleTx result() const { return tx_; }
    const char* status() const;
    const char* tx_status() const;
private:
    void release();
    BlePort& port_;
    BleLink link_{};
    bool enabled_=false, active_=false, pairing_=false, down_=false, needs_release_=true;
    bool error_=false;
    int64_t pair_deadline_=0, next_report_=0;
    char digits_[9]{};
    size_t position_=0, length_=0;
    BleTx tx_=BleTx::Idle;
};
BleKeyboard& bluetooth();
}
