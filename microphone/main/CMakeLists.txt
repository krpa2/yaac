idf_component_register(SRCS "main.c" "display.c" "scope_draw.c"
                       INCLUDE_DIRS "."
                       REQUIRES board_power esp_lcd esp_driver_spi esp_driver_i2s esp_driver_ledc
                                esp_driver_gpio esp_driver_i2c esp_timer)
