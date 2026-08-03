idf_component_register(SRCS "main.cpp"
                            "drivers/i2s_mic.cpp"
                            "drivers/button.cpp"
                            "drivers/wifi.cpp"
                            "drivers/rtasr.cpp"
                    INCLUDE_DIRS "."
                                  "drivers"
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_wifi esp_event esp_netif nvs_flash tcp_transport esp-tls mbedtls)
