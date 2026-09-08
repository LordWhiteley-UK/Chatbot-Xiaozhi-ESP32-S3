/**
 * app.h — shared types and inter-module API for the Xiaozhi voice client.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Device states, following the lifecycle in docs/websocket.md §6 */
typedef enum {
    APP_STATE_STARTING = 0,
    APP_STATE_WIFI_PROVISIONING,
    APP_STATE_ACTIVATING,
    APP_STATE_IDLE,
    APP_STATE_CONNECTING,
    APP_STATE_LISTENING,
    APP_STATE_SPEAKING,
    APP_STATE_ERROR,
} app_state_t;

/* Identity + endpoint info obtained from the OTA activation endpoint. */
typedef struct {
    char websocket_url[256];
    char token[256];
    char firmware_version[32];
    bool needs_activation;
    char activation_code[16];   /* shown on the OLED when not yet activated */
    /* MQTT+UDP transport credentials (docs/mqtt-udp.md §6.1) */
    char mqtt_endpoint[128];
    char mqtt_client_id[160];
    char mqtt_username[256];
    char mqtt_password[256];
    char mqtt_publish_topic[64];
    char mqtt_subscribe_topic[64];
} ota_info_t;

/* Result of the OTA exchange, shared with the session layer. */
extern ota_info_t g_ota_info;

/* Device identity (MAC-derived) */
const char *app_device_id(void);
const char *app_client_id(void);
const char *app_firmware_version(void);

/* Display */
void display_init(void);
void display_status_line(const char *state_text, const char *detail);
void display_text(const char *text);          /* STT / subtitle, wrapped */
void display_activation(const char *code);
void display_emotion(const char *emotion);

/* Audio */
typedef void (*audio_frame_cb_t)(const uint8_t *opus, size_t len);
typedef void (*audio_pcm_cb_t)(const int16_t *pcm, int nsamples);
typedef void (*audio_utter_end_cb_t)(void);
int audio_init(audio_frame_cb_t on_encoded_frame);
void audio_set_pcm_cb(audio_pcm_cb_t cb);        /* e.g. the wake-word engine */
void audio_set_utter_end_cb(audio_utter_end_cb_t cb);   /* end-of-speech (VAD) */
void audio_start_mic(void);
void audio_stop_mic(void);
void audio_play(const uint8_t *opus, size_t len, int sample_rate);
void audio_set_playback_rate(int sample_rate);  /* reconfigure I2S TX clock */
void audio_play_flush(void);         /* reset decode queue + ring + decoder */
void audio_clear_playback(void);     /* stop ring buffer output immediately  */
bool audio_is_playing(void);

/* Opus codec */
void opus_init(void);
void opus_decoder_setup(int sample_rate);

/* WiFi provisioning (SoftAP web form on first boot, NVS afterwards) */
void wifi_prov_start(void);   /* blocks until station connected */

/* OTA / activation */
int ota_fetch(ota_info_t *out);          /* blocking, ~seconds */
bool ota_is_activated(void);
void ota_clear(void);                    /* forget activation (debug) */

/* Session (WebSocket protocol, docs/websocket.md) */
int session_start(void);      /* connect + hello handshake */
void session_stop(void);
void session_start_listening(void);
void session_stop_listening(void);
void session_send_abort(void);
void session_send_audio(const uint8_t *opus, size_t len);
void session_send_mcp(const char *json_rpc_payload);
bool session_is_open(void);

/* MCP server (device-side tools), fed by the session layer */
void mcp_handle_payload(const char *payload_json, size_t len);
void session_init(void);

/* Local wake word (esp-sr wakenet, "Computer") */
int wake_word_init(void);
bool wake_word_ready(void);
const char *wake_word_name(void);
void wake_word_set_armed(bool armed);
void wake_word_feed(const int16_t *pcm, int nsamples);

/* App event queue (main.c owns it; protocol callbacks post into it) */
typedef enum {
    APP_EVENT_BTN_DOWN = 0,
    APP_EVENT_BTN_UP,
    APP_EVENT_STT,
    APP_EVENT_TTS_START,
    APP_EVENT_TTS_STOP,
    APP_EVENT_TTS_TEXT,
    APP_EVENT_EMOTION,
    APP_EVENT_ALERT,
    APP_EVENT_WAKE_WORD,
    APP_EVENT_UTTER_END,   /* device VAD: the user finished speaking */
    APP_EVENT_WS_CLOSED,
} app_event_t;
void app_post_event(app_event_t ev, const char *text);   /* copies text */

#ifdef __cplusplus
}
#endif