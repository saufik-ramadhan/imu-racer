idf_component_register(SRCS "main.c"
                            "i2c_bus.c"
                            "gy87.c"
                            "ssd1306.c"
                            "ble_controller.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2c bt nvs_flash)
