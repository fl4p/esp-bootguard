cmake_minimum_required(VERSION 3.16)

# The boot guard lives two levels up: the app component (bootguard) and the bootloader override.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# The BLE OTA receiver (https://github.com/fl4p/esp-ota-ble). Default: a checkout at ~/dev/pv/esp-ota-ble
# relative to this tree (esp-bootguard at ~/dev/esp/esp-bootguard). Override with -DESP_OTA_BLE_DIR=...
# or the ESP_OTA_BLE_DIR environment variable. A recovery app without its receiver is useless, so a
# missing checkout stops the build.
if(NOT ESP_OTA_BLE_DIR)
    if(DEFINED ENV{ESP_OTA_BLE_DIR})
        set(ESP_OTA_BLE_DIR "$ENV{ESP_OTA_BLE_DIR}")
    else()
        get_filename_component(ESP_OTA_BLE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../pv/esp-ota-ble" ABSOLUTE)
    endif()
endif()
if(NOT EXISTS "${ESP_OTA_BLE_DIR}/src/ota_ble.h")
    message(FATAL_ERROR "esp-ota-ble not found at ${ESP_OTA_BLE_DIR}: clone https://github.com/fl4p/esp-ota-ble "
            "and pass -DESP_OTA_BLE_DIR=<path> or set ESP_OTA_BLE_DIR.")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${ESP_OTA_BLE_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build the guarded bootloader too, so this project alone produces bootloader + partition table + app.
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../bootloader_components" APPEND)

project(recovery)

# esp-ota-ble's src/ota_ble.cpp includes esp_timer.h, but its CMakeLists.txt does not list esp_timer in
# REQUIRES (fugu-mppt-firmware gets the include path leaked through arduino-esp32). Add it from here
# rather than editing the receiver checkout.
idf_component_get_property(_ota_ble_lib esp-ota-ble COMPONENT_LIB)
target_link_libraries(${_ota_ble_lib} PUBLIC idf::esp_timer)
