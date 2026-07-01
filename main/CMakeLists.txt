idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES APP_OTA AUDIO LED KEY LCD WIFI VOICE_UPLOAD)
