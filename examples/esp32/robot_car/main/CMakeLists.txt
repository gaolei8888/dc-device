idf_component_register(
    SRCS "main.cpp" "../../../platforms/esp32/src/dc_device_esp32.cpp"
    INCLUDE_DIRS "../../../platforms/esp32/include"
    REQUIRES esp_http_server esp_event esp_netif nvs_flash driver
)
