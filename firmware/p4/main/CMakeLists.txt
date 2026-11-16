idf_component_register(SRCS "main.c" "console.c" "usb.c" "c5.c" "bridge.c" "pins.c" "iq.c" "still.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_parlio esp_hal_dma esp_mm esp_timer esp_hw_support esp_app_format
                                     espressif__tinyusb)

# TinyUSB reads its configuration from tusb_config.h on its own include path.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tusb")
