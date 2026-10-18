#pragma once
#include "esp_err.h"
struct tinyusb_config_t { void* device_descriptor;void* string_descriptor;bool external_phy;void* configuration_descriptor; };
constexpr int TINYUSB_USBDEV_0=0,TINYUSB_CDC_ACM_0=0;
esp_err_t tinyusb_driver_install(const tinyusb_config_t*);
