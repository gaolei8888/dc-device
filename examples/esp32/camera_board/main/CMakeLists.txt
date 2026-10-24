idf_component_register(
    SRCS "main.cpp"
         "../../../../platforms/esp32/src/dc_device_esp32.cpp"
         "../../../../platforms/esp32/src/dc_device_wifi.cpp"
         "../../../../platforms/esp32/src/dc_device_wifi_manager.cpp"
         "../../../../platforms/esp32/src/dc_device_declared_parts.cpp"
         "../../../../platforms/esp32/src/dc_device_parts_nvs.cpp"
         "../../../../platforms/esp32/src/dc_device_base64.cpp"
         "../../../../platforms/esp32/src/dc_device_screen_blocks.cpp"
         "../../../../platforms/esp32/src/dc_device_setup.cpp"
         "../../../../platforms/esp32/src/dc_device_setup_console.cpp"
         "../../../../platforms/esp32/src/dc_device_json.cpp"
         "../../../../platforms/esp32/src/dc_device_chip.cpp"
         "../../../../platforms/esp32/src/dc_device_selfcheck.cpp"
         "../../../../platforms/esp32/src/dc_device_pins.cpp"
         "../../../../platforms/esp32/src/dc_device_i2c_ids.cpp"
         "../../../../platforms/esp32/src/dc_device_i2c_detect.cpp"
         "../../../../platforms/esp32/src/dc_device_pin_survey.cpp"
         "../../../../platforms/esp32/src/dc_device_audio_analysis.cpp"
         "../../../../platforms/esp32/src/dc_device_image_analysis.cpp"
         "../../../../platforms/esp32/src/dc_device_mic_detect.cpp"
         "../../../../platforms/esp32/src/dc_device_endpoint.cpp"
         "../../../../platforms/esp32/src/dc_device_http_server.cpp"
    INCLUDE_DIRS "../../../../platforms/esp32/include"
    REQUIRES esp_http_server esp_event esp_netif esp_wifi nvs_flash driver freertos log spi_flash esp_hw_support esp_system esp_timer esp_psram mbedtls esp32-camera espressif__esp_jpeg
)

# scripts/build-release.sh passes -DDC_DEVICE_VERSION=0.1.0+<git commit>. A build made by
# hand reports 0.1.0+dev, which tells a person it is not a release.
if(DEFINED DC_DEVICE_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DC_DEVICE_VERSION="${DC_DEVICE_VERSION}")
endif()
