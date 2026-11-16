idf_component_register(SRCS "main.c" "console.c" "usb.c" "c5.c" "bridge.c" "pins.c" "iq.c" "still.c" "decode.c" "video.c"
                            "demod.S" "chroma.S" "deint.S" "despeck.S" "jumps.S" "luma.S" "tnr.S" "yuv420.S"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_parlio esp_driver_bitscrambler esp_driver_dma esp_hal_dma esp_mm esp_timer esp_hw_support esp_app_format
                                     espressif__tinyusb)

# TinyUSB reads its configuration from tusb_config.h on its own include path.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tusb")

target_bitscrambler_add_src("bs_phase.bsasm")

# iq.c puts a long capture's descriptor list in PSRAM.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=gdma_new_link_list")
