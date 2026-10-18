#include "ble_keyboard.h"
#include "ble_report.h"
#include "model.h"
#include "board.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_hid_common.h>
#include <esp_timer.h>
#include <nvs.h>
#include <tusb.h>
#include <tinyusb_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <cstring>

namespace otp {
namespace {
static constexpr auto& report_map=BleKeyboardReportMap;
enum Attribute { Service,IncludedBattery,InfoChar,Info,ControlChar,Control,ProtocolChar,Protocol,
    MapChar,Map,InputChar,Input,InputCcc,InputRef,OutputChar,Output,OutputRef,
    BootChar,Boot,BootCcc,BootOutputChar,BootOutput,AttributeCount };
static const uint16_t primary=ESP_GATT_UUID_PRI_SERVICE, declaration=ESP_GATT_UUID_CHAR_DECLARE,
    ccc=ESP_GATT_UUID_CHAR_CLIENT_CONFIG, reference=ESP_GATT_UUID_RPT_REF_DESCR, included=ESP_GATT_UUID_INCLUDE_SERVICE;
static const uint16_t hid_uuid=ESP_GATT_UUID_HID_SVC, info_uuid=ESP_GATT_UUID_HID_INFORMATION,
    control_uuid=ESP_GATT_UUID_HID_CONTROL_POINT, protocol_uuid=ESP_GATT_UUID_HID_PROTO_MODE,
    map_uuid=ESP_GATT_UUID_HID_REPORT_MAP, report_uuid=ESP_GATT_UUID_HID_REPORT,
    boot_uuid=ESP_GATT_UUID_HID_BT_KB_INPUT, boot_output_uuid=ESP_GATT_UUID_HID_BT_KB_OUTPUT;
// GAP's advertising API requires complete 128-bit UUIDs, even for an assigned
// 16-bit service. Bluetooth Base UUID + HID (0x1812), in little-endian order.
static const uint8_t hid_adv_uuid[]={
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,
    static_cast<uint8_t>(hid_uuid),static_cast<uint8_t>(hid_uuid>>8),0x00,0x00
};
static const uint8_t read_property=ESP_GATT_CHAR_PROP_BIT_READ,
    write_property=ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
    rw_property=ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
    output_property=ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE|ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
    input_property=ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t hid_info[]={0x11,0x01,0x00,0x02}, input_ref[]={1,1}, output_ref[]={1,2};
static uint8_t protocol=1, control=0, leds=0, input_value[8]{}, zero_ccc[2]{};
static esp_gatts_attr_db_t database[AttributeCount];
static const uint16_t dis_uuid=ESP_GATT_UUID_DEVICE_INFO_SVC, manufacturer_uuid=ESP_GATT_UUID_MANU_NAME,
    pnp_uuid=ESP_GATT_UUID_PNP_ID, battery_uuid=ESP_GATT_UUID_BATTERY_SERVICE_SVC, level_uuid=ESP_GATT_UUID_BATTERY_LEVEL;
static const char manufacturer[]="M5Stack";
// Identify the same physical device as the existing SDK USB CDC descriptor.
#if CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID
static constexpr uint16_t vendor_id=USB_ESPRESSIF_VID;
#else
static constexpr uint16_t vendor_id=CONFIG_TINYUSB_DESC_CUSTOM_VID;
#endif
#if CONFIG_TINYUSB_DESC_USE_DEFAULT_PID
static constexpr uint16_t product_id=0x4000|CFG_TUD_CDC|(CFG_TUD_MSC<<1)|(CFG_TUD_HID<<2)|
    (CFG_TUD_MIDI<<3)|(CFG_TUD_AUDIO<<4)|(CFG_TUD_VENDOR<<5);
#else
static constexpr uint16_t product_id=CONFIG_TINYUSB_DESC_CUSTOM_PID;
#endif
static const uint8_t pnp[]={2,static_cast<uint8_t>(vendor_id),static_cast<uint8_t>(vendor_id>>8),
    static_cast<uint8_t>(product_id),static_cast<uint8_t>(product_id>>8),
    static_cast<uint8_t>(CONFIG_TINYUSB_DESC_BCD_DEVICE),static_cast<uint8_t>(CONFIG_TINYUSB_DESC_BCD_DEVICE>>8)};
static esp_gatts_attr_db_t device_info[5],battery_service[4];
static esp_gatts_incl_svc_desc_t included_battery{};
static uint8_t battery_level=0;

static void attribute(esp_gatts_attr_db_t& out,const uint16_t& uuid,uint16_t permission,
                      uint16_t max_length,uint16_t length,const void* value) {
    out={}; out.attr_control.auto_rsp=ESP_GATT_AUTO_RSP;
    out.att_desc.uuid_length=ESP_UUID_LEN_16;
    out.att_desc.uuid_p=reinterpret_cast<uint8_t*>(const_cast<uint16_t*>(&uuid));
    out.att_desc.perm=permission; out.att_desc.max_length=max_length; out.att_desc.length=length;
    out.att_desc.value=const_cast<uint8_t*>(static_cast<const uint8_t*>(value));
}
static void make_database() {
    const uint16_t read=ESP_GATT_PERM_READ, secure_read=ESP_GATT_PERM_READ_ENC_MITM,
        secure_write=ESP_GATT_PERM_WRITE_ENC_MITM;
    auto add=[](Attribute i,const uint16_t& uuid,uint16_t perm,uint16_t cap,uint16_t n,const void* p){attribute(database[i],uuid,perm,cap,n,p);};
    add(Service,primary,read,2,2,&hid_uuid);
    add(IncludedBattery,included,read,sizeof included_battery,sizeof included_battery,&included_battery);
    add(InfoChar,declaration,read,1,1,&read_property); add(Info,info_uuid,read,4,4,hid_info);
    add(ControlChar,declaration,read,1,1,&write_property); add(Control,control_uuid,secure_write,1,1,&control);
    add(ProtocolChar,declaration,read,1,1,&rw_property); add(Protocol,protocol_uuid,secure_read|secure_write,1,1,&protocol);
    add(MapChar,declaration,read,1,1,&read_property); add(Map,map_uuid,read,sizeof report_map,sizeof report_map,report_map);
    add(InputChar,declaration,read,1,1,&input_property); add(Input,report_uuid,secure_read,8,8,input_value);
    add(InputCcc,ccc,secure_read|secure_write,2,2,zero_ccc); add(InputRef,reference,read,2,2,input_ref);
    add(OutputChar,declaration,read,1,1,&output_property); add(Output,report_uuid,secure_read|secure_write,1,1,&leds);
    add(OutputRef,reference,read,2,2,output_ref);
    add(BootChar,declaration,read,1,1,&input_property); add(Boot,boot_uuid,secure_read,8,8,input_value);
    add(BootCcc,ccc,secure_read|secure_write,2,2,zero_ccc);
    add(BootOutputChar,declaration,read,1,1,&output_property); add(BootOutput,boot_output_uuid,secure_read|secure_write,1,1,&leds);
    attribute(device_info[0],primary,read,2,2,&dis_uuid);
    attribute(device_info[1],declaration,read,1,1,&read_property);
    attribute(device_info[2],manufacturer_uuid,read,sizeof manufacturer-1,sizeof manufacturer-1,manufacturer);
    attribute(device_info[3],declaration,read,1,1,&read_property);
    attribute(device_info[4],pnp_uuid,read,sizeof pnp,sizeof pnp,pnp);
    attribute(battery_service[0],primary,read,2,2,&battery_uuid);
    attribute(battery_service[1],declaration,read,1,1,&input_property);
    attribute(battery_service[2],level_uuid,read,1,1,&battery_level);
    attribute(battery_service[3],ccc,secure_read|secure_write,2,2,zero_ccc);
}

class EspBlePort final : public BlePort {
public:
    bool load(bool& enabled) override;
    bool save_enabled(bool enabled) override;
    bool start() override;
    void stop() override;
    BleLink link() override;
    void pairing(bool allow) override;
    void confirm(bool yes) override;
    bool forget() override;
    bool report(const uint8_t* data,size_t length) override;
    void gap(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t* p);
    void gatt(esp_gatts_cb_event_t event,esp_gatt_if_t iface,esp_ble_gatts_cb_param_t* p);
private:
    bool setting(const char* key,uint32_t value);
    void advertise(); // called under mutex; GAP operations themselves are async
    void disconnect();
    void changed() { ++state_.generation; }
    std::mutex mutex_;
    BleLink state_{};
    bool running_=false, pairing_=false, configured_=false, service_=false;
    BlePairingGate gate_;
    bool report_ccc_=false, boot_ccc_=false, boot_mode_=false, suspended_=false;
    bool advertising_=false, removing_=false, removal_failed_=false;
    int pending_removals_=0;
    uint32_t saved_ccc_=0;
    esp_gatt_if_t interface_=ESP_GATT_IF_NONE;
    uint16_t connection_=0, handles_[AttributeCount]{};
    uint16_t info_service_=0;
    uint16_t battery_handles_[4]{};
    bool battery_ccc_=false;
    uint8_t started_=0;
    esp_bd_addr_t peer_{};
};
static EspBlePort port;
static void gap_callback(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t* p) { port.gap(event,p); }
static void gatt_callback(esp_gatts_cb_event_t event,esp_gatt_if_t iface,esp_ble_gatts_cb_param_t* p) { port.gatt(event,iface,p); }

BleLink EspBlePort::link() {
    std::lock_guard<std::mutex> guard(mutex_);
    uint16_t mv=board::battery_mv();
    // BAS needs a percentage. This is a voltage estimate, not a calibrated
    // fuel-gauge reading; no claim about battery life is derived from it.
    uint8_t level=mv>=4200?100:mv<=3300?0:static_cast<uint8_t>((mv-3300)/9);
    if(running_ && battery_handles_[2] && level!=battery_level) {
        battery_level=level;
        esp_ble_gatts_set_attr_value(battery_handles_[2],1,&battery_level);
        if(state_.secure && battery_ccc_) esp_ble_gatts_send_indicate(interface_,connection_,battery_handles_[2],1,&battery_level,false);
    }
    return state_;
}

bool EspBlePort::setting(const char* key,uint32_t value) {
    nvs_handle_t handle;
    if(nvs_open("ble_keyboard",NVS_READWRITE,&handle)!=ESP_OK) return false;
    bool ok=nvs_set_u32(handle,key,value)==ESP_OK && nvs_commit(handle)==ESP_OK;
    nvs_close(handle); return ok;
}
bool EspBlePort::load(bool& enabled) {
    nvs_handle_t handle;
    if(nvs_open("ble_keyboard",NVS_READWRITE,&handle)!=ESP_OK) return false;
    uint32_t on=0,bonded=0;
    esp_err_t a=nvs_get_u32(handle,"enabled",&on), b=nvs_get_u32(handle,"bonded",&bonded);
    nvs_close(handle);
    if ((a!=ESP_OK && a!=ESP_ERR_NVS_NOT_FOUND) || (b!=ESP_OK && b!=ESP_ERR_NVS_NOT_FOUND)) return false;
    enabled=on==1; state_.bonded=bonded==1; return true;
}
bool EspBlePort::save_enabled(bool enabled) { return setting("enabled",enabled?1:0); }

bool EspBlePort::start() {
    // esp_hid validates the public HID descriptor. Its device input_set API
    // waits on a semaphore without a timeout, so notifications use the public
    // GATTS API below instead; no Bluetooth operation blocks the UI for a report.
    auto* parsed=esp_hid_parse_report_map(report_map,sizeof report_map);
    if(!parsed) return false;
    bool valid=parsed->usage==ESP_HID_USAGE_KEYBOARD;
    bool input=false,output=false;
    for(uint8_t i=0;i<parsed->reports_len;++i) {
        const auto& r=parsed->reports[i];
        if(r.protocol_mode==ESP_HID_PROTOCOL_MODE_REPORT && r.report_id==1) {
            input|=r.report_type==ESP_HID_REPORT_TYPE_INPUT && r.value_len==8;
            output|=r.report_type==ESP_HID_REPORT_TYPE_OUTPUT && r.value_len==1;
        }
    }
    valid=valid&&input&&output;
    esp_hid_free_report_map(parsed); if(!valid) return false;
    esp_bt_controller_config_t config=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&config)!=ESP_OK) return false;
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE)!=ESP_OK) { stop(); return false; }
    if (esp_bluedroid_init()!=ESP_OK || esp_bluedroid_enable()!=ESP_OK) { stop(); return false; }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        running_=true; state_.fault=false; state_.connected=state_.secure=state_.subscribed=state_.comparison=false;
        int bonds=esp_ble_get_bond_device_num();
        state_.bonded=bonds>0;state_.fault=bonds<0 || bonds>1;
        configured_=service_=advertising_=pairing_=false;gate_.close();
        report_ccc_=boot_ccc_=boot_mode_=suspended_=removing_=false; pending_removals_=0;
        changed(); make_database();started_=0;info_service_=0;std::memset(battery_handles_,0,sizeof battery_handles_);battery_ccc_=false;
        nvs_handle_t h;
        saved_ccc_=0;
        if(nvs_open("ble_keyboard",NVS_READONLY,&h)==ESP_OK) {
            nvs_get_u32(h,"ccc",&saved_ccc_);nvs_close(h);
        }
        if(!state_.bonded) saved_ccc_=0;
    }
    uint8_t auth=ESP_LE_AUTH_REQ_SC_MITM_BOND, io=ESP_IO_CAP_IO, key_size=16;
    uint8_t keys=ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK, only_auth=ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    bool ok=esp_ble_gap_register_callback(gap_callback)==ESP_OK &&
        esp_ble_gatts_register_callback(gatt_callback)==ESP_OK &&
        esp_ble_gap_set_device_name("StopWatch OTP")==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,&auth,sizeof auth)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,&io,sizeof io)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,&key_size,sizeof key_size)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE,&key_size,sizeof key_size)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,&keys,sizeof keys)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,&keys,sizeof keys)==ESP_OK &&
        esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,&only_auth,sizeof only_auth)==ESP_OK &&
        esp_ble_gatts_app_register(0x55)==ESP_OK;
    if(!ok) { stop(); return false; }
    // Controller modem sleep maintains the BLE connection; manual CPU light
    // sleep is skipped by Ui while this port is active.
    esp_bt_sleep_enable(); return true;
}
void EspBlePort::stop() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pairing_=false; gate_.close();
        if(running_) { esp_ble_gap_stop_advertising(); disconnect(); }
        running_=false; state_.connected=state_.secure=state_.subscribed=state_.comparison=state_.fault=false; changed();
    }
    if (esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
    if (esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_INITIALIZED) esp_bluedroid_deinit();
    if (esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
    if (esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
}
void EspBlePort::disconnect() {
    state_.secure=state_.subscribed=state_.comparison=false; changed();
    if (state_.connected) esp_ble_gap_disconnect(peer_);
}
void EspBlePort::advertise() {
    if(!running_ || !configured_ || !service_ || state_.connected || advertising_ || removing_ || state_.fault || (!state_.bonded && !pairing_)) return;
    esp_ble_adv_params_t params{};
    params.adv_int_min=0x30; params.adv_int_max=0x60;
    params.adv_type=ADV_TYPE_IND; params.own_addr_type=BLE_ADDR_TYPE_PUBLIC;
    params.channel_map=ADV_CHNL_ALL; params.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    advertising_=esp_ble_gap_start_advertising(&params)==ESP_OK;
    if(!advertising_) state_.fault=true;
}
void EspBlePort::pairing(bool allow) {
    std::lock_guard<std::mutex> guard(mutex_);
    pairing_=allow && !state_.bonded && running_ && !removing_;
    if(!pairing_) {
        gate_.close();
        if(state_.comparison) esp_ble_confirm_reply(peer_,false);
        state_.comparison=false;
        if(!state_.secure) disconnect();
        if(!state_.bonded) { esp_ble_gap_stop_advertising(); advertising_=false; }
    } else { gate_.open(esp_timer_get_time()); advertise(); }
}
void EspBlePort::confirm(bool yes) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(!gate_.compare(esp_timer_get_time(),state_.bonded) || !state_.comparison || !state_.connected) return;
    gate_.approved=yes; state_.comparison=false;
    if(esp_ble_confirm_reply(peer_,yes)!=ESP_OK || !yes) disconnect();
}
bool EspBlePort::forget() {
    std::unique_lock<std::mutex> guard(mutex_);
    if(!running_) return false;
    pairing_=false;gate_.close(); removing_=true;removal_failed_=false;
    esp_ble_gap_stop_advertising(); advertising_=false; disconnect();
    int count=esp_ble_get_bond_device_num();
    if(count<0 || count>16) { state_.fault=true; return false; }
    esp_ble_bond_dev_t bonds[16]{};
    if(count && esp_ble_get_bond_device_list(&count,bonds)!=ESP_OK) return false;
    pending_removals_=count;
    bool ok=true;
    for(int i=0;i<count;++i) if(esp_ble_remove_bond_device(bonds[i].bd_addr)!=ESP_OK) { ok=false; --pending_removals_; }
    wipe(bonds,sizeof bonds);
    if(!count) { state_.bonded=false; removing_=false; saved_ccc_=0;ok=setting("bonded",0)&&setting("ccc",0); }
    if(!ok) state_.fault=true;
    guard.unlock();
    // Bond removal is asynchronous. Do not power the stack down or claim that
    // the PC has been forgotten before its NVS keys have actually been removed.
    for(int i=0;i<200;++i) {
        {
            std::lock_guard<std::mutex> check(mutex_);
            if(!removing_) return ok && !state_.bonded && !removal_failed_;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    guard.lock();state_.fault=true;return false;
}
bool EspBlePort::report(const uint8_t* data,size_t length) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(!running_ || !state_.connected || !state_.secure || !state_.subscribed || state_.fault || length!=8) return false;
    uint16_t handle=handles_[boot_mode_?Boot:Input];
    bool ok=handle && esp_ble_gatts_send_indicate(interface_,connection_,handle,length,const_cast<uint8_t*>(data),false)==ESP_OK;
    if(!ok) disconnect();
    return ok;
}
void EspBlePort::gap(esp_gap_ble_cb_event_t event,esp_ble_gap_cb_param_t* p) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(!running_) return;
    switch(event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            configured_=p->adv_data_cmpl.status==ESP_BT_STATUS_SUCCESS;
            if(!configured_) state_.fault=true;
            advertise(); break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if(p->adv_start_cmpl.status!=ESP_BT_STATUS_SUCCESS) { advertising_=false; state_.fault=true; }
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr,
                !removing_ && (gate_.live(esp_timer_get_time()) || state_.bonded)); break;
        case ESP_GAP_BLE_NC_REQ_EVT:
            if(gate_.compare(esp_timer_get_time(),state_.bonded) && state_.connected && !std::memcmp(peer_,p->ble_security.key_notif.bd_addr,6)) {
                state_.comparison=true; state_.number=p->ble_security.key_notif.passkey;
            } else esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr,false);
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            // DisplayYesNo is required. Never silently fall back to Just Works
            // or a fixed PIN on an incompatible host.
            esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr,false,0); disconnect(); break;
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            disconnect(); break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            const auto& auth=p->ble_security.auth_cmpl;
            bool protected_link=(auth.auth_mode & ESP_LE_AUTH_REQ_SC_MITM)==ESP_LE_AUTH_REQ_SC_MITM;
            bool allowed=state_.connected && !std::memcmp(peer_,auth.bd_addr,6) && !removing_;
            int count=esp_ble_get_bond_device_num();
            if(allowed && gate_.authenticate(esp_timer_get_time(),state_.bonded,auth.success,protected_link,count) && setting("bonded",1)) {
                state_.secure=true; state_.bonded=true; state_.comparison=false;
                state_.subscribed=!suspended_ && (boot_mode_?boot_ccc_:report_ccc_);
            } else {
                // Remove a newly created, cancelled, or unauthenticated bond;
                // never replace the already trusted computer implicitly.
                if(!state_.bonded && auth.success) esp_ble_remove_bond_device(p->ble_security.auth_cmpl.bd_addr);
                disconnect();
            }
            break;
        }
        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
            if(removing_) {
                if(p->remove_bond_dev_cmpl.status!=ESP_BT_STATUS_SUCCESS) state_.fault=removal_failed_=true;
                if(pending_removals_>0) --pending_removals_;
                if(!pending_removals_) {
                    state_.bonded=esp_ble_get_bond_device_num()!=0; removing_=false;
                    if(!state_.bonded) saved_ccc_=0;
                    if(!setting("bonded",state_.bonded?1:0) || !setting("ccc",saved_ccc_) || state_.bonded) state_.fault=removal_failed_=true;
                }
            }
            break;
        default: break;
    }
}
void EspBlePort::gatt(esp_gatts_cb_event_t event,esp_gatt_if_t iface,esp_ble_gatts_cb_param_t* p) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(!running_) return;
    switch(event) {
        case ESP_GATTS_REG_EVT: {
            if(p->reg.status!=ESP_GATT_OK) { state_.fault=true; break; }
            interface_=iface;
            esp_ble_adv_data_t adv{}; adv.include_name=true;
            adv.appearance=ESP_HID_APPEARANCE_KEYBOARD;
            adv.service_uuid_len=sizeof hid_adv_uuid; adv.p_service_uuid=const_cast<uint8_t*>(hid_adv_uuid);
            adv.flag=ESP_BLE_ADV_FLAG_GEN_DISC|ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
            if(esp_ble_gap_config_adv_data(&adv)!=ESP_OK || esp_ble_gatts_create_attr_tab(battery_service,iface,4,0)!=ESP_OK) state_.fault=true;
            break;
        }
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if(p->add_attr_tab.svc_uuid.len==ESP_UUID_LEN_16 && p->add_attr_tab.svc_uuid.uuid.uuid16==battery_uuid) {
                if(p->add_attr_tab.status!=ESP_GATT_OK || p->add_attr_tab.num_handle!=4) { state_.fault=true;break; }
                std::memcpy(battery_handles_,p->add_attr_tab.handles,sizeof battery_handles_);
                included_battery.start_hdl=battery_handles_[0];included_battery.end_hdl=battery_handles_[3];
                if(esp_ble_gatts_start_service(battery_handles_[0])!=ESP_OK ||
                   esp_ble_gatts_create_attr_tab(device_info,iface,5,1)!=ESP_OK) state_.fault=true;
                break;
            }
            if(p->add_attr_tab.svc_uuid.len==ESP_UUID_LEN_16 && p->add_attr_tab.svc_uuid.uuid.uuid16==dis_uuid) {
                if(p->add_attr_tab.status!=ESP_GATT_OK || p->add_attr_tab.num_handle!=5) { state_.fault=true;break; }
                info_service_=p->add_attr_tab.handles[0];
                if(esp_ble_gatts_start_service(info_service_)!=ESP_OK ||
                   esp_ble_gatts_create_attr_tab(database,iface,AttributeCount,2)!=ESP_OK) state_.fault=true;
                break;
            }
            if(p->add_attr_tab.status!=ESP_GATT_OK || p->add_attr_tab.num_handle!=AttributeCount) { state_.fault=true; break; }
            std::memcpy(handles_,p->add_attr_tab.handles,sizeof handles_);
            if(esp_ble_gatts_start_service(handles_[Service])!=ESP_OK) state_.fault=true;
            break;
        case ESP_GATTS_START_EVT:
            if(p->start.status!=ESP_GATT_OK) state_.fault=true;
            else if(p->start.service_handle==handles_[Service]) started_|=1;
            else if(p->start.service_handle==info_service_) started_|=2;
            else if(p->start.service_handle==battery_handles_[0]) started_|=4;
            service_=started_==7;advertise();break;
        case ESP_GATTS_CONNECT_EVT: {
            advertising_=false;boot_mode_=suspended_=false;gate_.approved=false;
            report_ccc_=state_.bonded && (saved_ccc_&1);boot_ccc_=state_.bonded && (saved_ccc_&2);
            battery_ccc_=state_.bonded && (saved_ccc_&4);
            state_.connected=true; state_.secure=state_.subscribed=state_.comparison=false; changed();
            connection_=p->connect.conn_id; std::memcpy(peer_,p->connect.remote_bda,6);
            // CCCs belong to the bonded host and must survive reconnect/reboot.
            // Readiness remains false until that host authenticates again.
            uint8_t report_ccc[2]={static_cast<uint8_t>(report_ccc_),0},boot_ccc[2]={static_cast<uint8_t>(boot_ccc_),0};
            esp_ble_gatts_set_attr_value(handles_[InputCcc],2,report_ccc);
            esp_ble_gatts_set_attr_value(handles_[BootCcc],2,boot_ccc);
            uint8_t battery_ccc[2]={static_cast<uint8_t>(battery_ccc_),0};
            esp_ble_gatts_set_attr_value(battery_handles_[3],2,battery_ccc);
            const uint8_t report_protocol=1; esp_ble_gatts_set_attr_value(handles_[Protocol],1,&report_protocol);
            esp_ble_gatts_set_attr_value(handles_[Input],8,input_value);
            esp_ble_gatts_set_attr_value(handles_[Boot],8,input_value);
            if(removing_ || (!gate_.live(esp_timer_get_time()) && !state_.bonded)) disconnect();
            else if(esp_ble_set_encryption(peer_,ESP_BLE_SEC_ENCRYPT_MITM)!=ESP_OK) disconnect();
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT:
            state_.connected=state_.secure=state_.subscribed=state_.comparison=false;
            report_ccc_=boot_ccc_=battery_ccc_=false; gate_.approved=false; changed(); advertise(); break;
        case ESP_GATTS_WRITE_EVT:
            if(p->write.is_prep || p->write.conn_id!=connection_ || !state_.secure) break;
            if(p->write.handle==handles_[InputCcc] || p->write.handle==handles_[BootCcc] || p->write.handle==battery_handles_[3]) {
                bool on=p->write.len==2 && p->write.value[0]==1 && p->write.value[1]==0;
                if(p->write.handle==handles_[InputCcc]) report_ccc_=on;
                else if(p->write.handle==handles_[BootCcc]) boot_ccc_=on; else battery_ccc_=on;
                uint32_t mask=(report_ccc_?1u:0u)|(boot_ccc_?2u:0u)|(battery_ccc_?4u:0u);
                if(mask!=saved_ccc_) {
                    if(setting("ccc",mask)) saved_ccc_=mask;
                    else {state_.fault=true;disconnect();break;}
                }
            } else if(p->write.handle==handles_[Protocol] && p->write.len==1) {
                boot_mode_=p->write.value[0]==0; changed();
            } else if(p->write.handle==handles_[Control] && p->write.len==1) {
                suspended_=p->write.value[0]==0; changed();
            }
            if(state_.subscribed && !(!suspended_ && (boot_mode_?boot_ccc_:report_ccc_))) changed();
            state_.subscribed=!suspended_ && (boot_mode_?boot_ccc_:report_ccc_); break;
        case ESP_GATTS_CONF_EVT:
            // A transport failure aborts this input but must not permanently
            // disable advertising/reconnection to the already bonded host.
            if(p->conf.status!=ESP_GATT_OK) disconnect();
            break;
        default: break;
    }
}
}
BleKeyboard& bluetooth() { static BleKeyboard keyboard(port); return keyboard; }
}
