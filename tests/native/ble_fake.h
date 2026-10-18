#pragma once
#include "ble_keyboard.h"
#include "ble_report.h"
#include <esp_hid_common.h>
#include <array>
#include <vector>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>

struct FakeBlePort : otp::BlePort {
    otp::BleLink state{};
    bool saved=false, running=false, pair_open=false, fail=false, approved=false;
    std::vector<std::array<uint8_t,8>> reports;
    bool load(bool& enabled) override { enabled=saved;return !fail; }
    bool save_enabled(bool on) override {if(fail)return false;saved=on;return true;}
    bool start() override {running=!fail;return running;}
    void stop() override {running=false;disconnect();}
    otp::BleLink link() override {return state;}
    void pairing(bool allow) override {pair_open=allow;state.comparison=false;}
    void confirm(bool yes) override {approved=yes;state.comparison=false;}
    bool forget() override {state.bonded=false;disconnect();return !fail;}
    bool report(const uint8_t* data,size_t length) override {
        if(fail || !running || !state.connected || !state.secure || !state.subscribed || length!=8)return false;
        std::array<uint8_t,8> r{};std::memcpy(r.data(),data,8);reports.push_back(r);return true;
    }
    void connect() {state.connected=state.secure=state.subscribed=state.bonded=true;state.fault=false;}
    void disconnect() {state.connected=state.secure=state.subscribed=state.comparison=false;++state.generation;}
    std::string typed() const {
        std::string out;
        for(const auto& r:reports) {
            assert(r[0]==0 && r[1]==0);
            for(size_t i=3;i<8;++i)assert(r[i]==0);
            if(r[2]) {assert(r[2]>=0x1e&&r[2]<=0x27);out+=r[2]==0x27?'0':static_cast<char>('1'+r[2]-0x1e);}
        }
        return out;
    }
};
static void ble_controller_tests() {
    using namespace otp;
    auto* map=esp_hid_parse_report_map(BleKeyboardReportMap,sizeof BleKeyboardReportMap);
    assert(map&&map->usage==ESP_HID_USAGE_KEYBOARD&&map->reports_len==4);
    unsigned reports=0;
    for(uint8_t i=0;i<map->reports_len;++i) {
        const auto& r=map->reports[i];assert(r.report_id==1&&r.usage==ESP_HID_USAGE_KEYBOARD);
        assert(r.report_type==ESP_HID_REPORT_TYPE_INPUT?r.value_len==8:r.value_len==1);
        reports|=1u<<((r.report_type==ESP_HID_REPORT_TYPE_INPUT?0:1)+(r.protocol_mode==ESP_HID_PROTOCOL_MODE_BOOT?2:0));
    }
    assert(reports==15);esp_hid_free_report_map(map);
    BlePairingGate gate;
    assert(!gate.authenticate(1,false,true,true,1));
    assert(!gate.compare(1,false));gate.open(1);
    assert(gate.compare(2,false)&&!gate.compare(2,true));
    assert(!gate.authenticate(2,false,true,true,1)); // numeric comparison has not been accepted
    gate.approved=true;
    assert(gate.authenticate(2,false,true,true,1));
    assert(!gate.authenticate(2,false,true,false,1)&&!gate.authenticate(2,false,false,true,1));
    assert(!gate.authenticate(2,false,true,true,2)&&!gate.authenticate(2,false,true,true,0));
    assert(!gate.authenticate(60000001,false,true,true,1));gate.close();
    assert(!gate.authenticate(2,false,true,true,1)&&gate.authenticate(2,true,true,true,1));
    FakeBlePort p;BleKeyboard k(p);assert(k.begin()&&!k.enabled()&&!k.active());
    assert(!k.send("001122",0)&&p.reports.empty());assert(k.enable(true)&&p.saved);
    p.state.connected=true;k.tick(0,true);assert(!k.send("001122",0));
    p.state.secure=true;k.tick(0,true);assert(!k.send("001122",0));
    p.connect();k.tick(0,true);p.reports.clear();
    assert(!k.send("12x456",0)&&!k.send("12345",0)&&!k.send("1234567",0));
    assert(k.send("001122",0));assert(!k.send("999999",0));
    for(int i=0;i<12;++i)k.tick(i*20000,true);
    assert(k.result()==BleTx::Sent&&p.typed()=="001122"&&p.reports.size()==12);
    for(size_t i=0;i<p.reports.size();i+=2)assert(p.reports[i][2]&&p.reports[i+1][2]==0);
    p.reports.clear();assert(k.send("00001111",300000));
    for(int i=0;i<16;++i)k.tick(300000+i*20000,true);
    assert(p.typed()=="00001111"&&k.result()==BleTx::Sent);
    // A disconnect/reconnect between ticks cannot resume an old request.
    p.reports.clear();assert(k.send("123456",700000));k.tick(700000,true);
    p.disconnect();p.connect();k.tick(720000,true);
    assert(k.result()==BleTx::Failed&&p.typed()=="1");
    k.tick(1000000,true);assert(p.typed()=="1");
    assert(k.send("654321",1000000));k.tick(1000000,true);k.tick(1020000,false);
    assert(k.result()==BleTx::Cancelled&&p.reports.back()[2]==0);
    assert(k.send("654321",1100000));p.fail=true;k.tick(1100000,true);assert(k.result()==BleTx::Failed);
    p.fail=false;k.cancel();assert(k.forget()&&!k.bonded());
    assert(k.pair(2000000));p.state.comparison=true;p.state.number=123;
    k.tick(3000000,true);assert(k.comparison()&&k.number()==123&&!p.approved);
    k.confirm(false);assert(!k.pairing()&&!p.approved);
    assert(k.pair(4000000));k.tick(64000000,true);assert(!k.pairing()&&!p.pair_open);
    assert(k.pair(65000000));k.tick(65000001,false);assert(!k.pairing());
    assert(k.pair(66000000));p.state.comparison=true;k.tick(66000001,true);k.confirm(true);assert(p.approved);
    p.connect();k.tick(66000002,true);assert(!k.pairing()&&k.ready());
    BleKeyboard reboot(p);assert(reboot.begin()&&reboot.enabled()&&reboot.bonded());
    assert(reboot.reset()&&!p.saved&&!p.running&&!reboot.bonded());
    assert(!reboot.send("123456",70000000));
    puts("PASS: BLE digit reports, release, leading zeroes, busy, security/subscription gates, cancellation, pairing timeout and persistence");
}
