idf_component_register(SRCS "app_main.c" "usb_safe.cpp" INCLUDE_DIRS "."
                       REQUIRES esp_netif esp_event nvs_flash
                       PRIV_REQUIRES lwip esp_driver_gpio)

target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)