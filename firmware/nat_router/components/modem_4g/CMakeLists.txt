idf_component_register(SRCS "modem_4g.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lwip esp_timer driver esp_netif oled_display)