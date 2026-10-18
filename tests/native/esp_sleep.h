#pragma once
enum esp_sleep_wakeup_cause_t { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_GPIO };
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
