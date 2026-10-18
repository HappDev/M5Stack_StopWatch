idf_component_register(SRCS "main.cpp" "crypto.cpp" "pattern.cpp" "clock.cpp" "vault.cpp" "board.cpp" "ui.cpp" "protocol.cpp" "ble_keyboard.cpp" "ble_esp.cpp" "font_ru.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls nvs_flash esp_partition esp_timer esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_hw_support esp_pm json M5GFX M5PM1 M5IOE1 lvgl bootloader_support esp_tinyusb bt esp_hid)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
