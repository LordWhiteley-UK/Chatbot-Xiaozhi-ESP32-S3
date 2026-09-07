idf_component_register(
    SRCS "main.c"
         "board.c"
         "display.c"
         "audio.c"
         "opus_codec.c"
         "wifi_prov.c"
         "ota.c"
         "session.c"
         "session_mqtt.c"
         "wake_word.c"
         "mcp_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_http_server esp_http_client
                  esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s
                  esp_netif esp_event esp-tls espressif__mqtt mbedtls
                  esp_ringbuf esp_websocket_client esp-opus cjson
                  espressif__esp-sr)