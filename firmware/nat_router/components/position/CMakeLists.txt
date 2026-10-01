idf_component_register(SRCS "position.c"
                       "nmea.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_timer nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
