#include "board.h"
#include "protocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>

static otp::Vault vault;
static bool usb_authorized = false;
static otp::Ui ui(vault, usb_authorized);
static otp::Protocol protocol(vault, ui, usb_authorized);
extern "C" void app_main() {
    vault.begin();
    if(!board::begin()) { while(true) vTaskDelay(pdMS_TO_TICKS(1000)); }
    ui.begin();
    const bool usb_ok=protocol.begin();
    if(!usb_ok) ui.message("Ошибка USB. Перезапустите устройство.");
    while(true) {
        board::tick(); lv_timer_handler(); ui.tick(); if(usb_ok) protocol.tick(); vTaskDelay(pdMS_TO_TICKS(10));
    }
}
