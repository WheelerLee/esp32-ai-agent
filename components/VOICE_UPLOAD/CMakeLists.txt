idf_component_register(SRCS "voice_upload.c"
                      INCLUDE_DIRS "include"
                      REQUIRES AUDIO esp_driver_i2s esp_timer esp_websocket_client json KEY WIFI)
