idf_component_register(SRCS "main.cpp" "ble_nus.c"
                       INCLUDE_DIRS "."
                       REQUIRES bootguard esp-ota-ble bt nvs_flash app_update esp_partition
                                esp_system esp_timer freertos log)
