idf_component_register(SRCS "audio.cpp"
                      INCLUDE_DIRS "include"
                      REQUIRES esp_driver_gpio esp_driver_i2s esp_http_client esp_timer micro-mp3 nvs_flash)
