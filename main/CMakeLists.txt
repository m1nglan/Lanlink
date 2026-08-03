idf_component_register(SRCS "main.cpp"
                            "drivers/wifi.cpp"
                            "drivers/i2s_mic.cpp"
                            "drivers/button.cpp"
                            "drivers/rtasr.cpp"
                    INCLUDE_DIRS "."
                                  "drivers"
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio esp_driver_i2s mbedtls tcp_transport)
