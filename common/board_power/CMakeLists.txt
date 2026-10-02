idf_component_register(SRCS "board_power.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_gpio esp_hw_support
                       PRIV_REQUIRES log)
