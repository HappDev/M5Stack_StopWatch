#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#define pdMS_TO_TICKS(x) (x)
struct tinyusb_config_cdcacm_t { int usb_dev,cdc_port;size_t rx_unread_buf_sz; };
esp_err_t tusb_cdc_acm_init(const tinyusb_config_cdcacm_t*);
size_t tinyusb_cdcacm_write_queue(int,const uint8_t*,size_t);
esp_err_t tinyusb_cdcacm_write_flush(int,int);
esp_err_t tinyusb_cdcacm_read(int,uint8_t*,size_t,size_t*);
