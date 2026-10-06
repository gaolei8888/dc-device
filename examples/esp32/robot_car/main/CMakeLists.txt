idf_component_register(
    SRCS "main.cpp"
         "../../../platforms/esp32/src/dc_device_esp32.cpp"
         "../../../platforms/esp32/src/dc_device_wifi.cpp"
         "../../../platforms/esp32/src/dc_device_json.cpp"
         "../../../platforms/esp32/src/dc_device_endpoint.cpp"
         "../../../platforms/esp32/src/dc_device_http_server.cpp"
    INCLUDE_DIRS "../../../platforms/esp32/include"
    REQUIRES esp_http_server esp_event esp_netif esp_wifi nvs_flash driver freertos log
)
