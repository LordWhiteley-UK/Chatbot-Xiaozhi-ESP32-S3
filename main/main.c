/**
 * main.c — app entry point and device state machine (manual mode).
 *
 * Flow: NVS -> display -> WiFi (provision if needed) -> OTA activation ->
 *       audio -> idle.  The on-board BOOT button (GPIO0) is push-to-talk:
 *       press = connect + listen, release = stop listening; TTS plays out
 *       through the amp, a second press while speaking aborts playback.
 *
 * State transitions follow docs/websocket.md §6.4 (manual mode):
 *   Idle -> Connecting -> Listening -> Speaking -> Idle
 */
#include "app.h"
#include "board.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "app";

typedef struct {
    app_event_t ev;
    char text[96];
} app_msg_t;

static QueueHandle_t s_events;
static volatile app_state_t s_state = APP_STATE_STARTING;

static const char *state_name(app_state_t s)
{
    switch (s) {
    case APP_STATE_STARTING:           return "Starting";
    case APP_STATE_WIFI_PROVISIONING:  return "WiFi setup";
    case APP_STATE_ACTIVATING:         return "Activating";
    case APP_STATE_IDLE:               return "Ready";
    case APP_STATE_CONNECTING:         return "Connecting";
    case APP_STATE_LISTENING:          return "Listening";
    case APP_STATE_SPEAKING:           return "Speaking";
    case APP_STATE_ERROR:              return "Error";
    default:                           return "?";
    }
}

static void set_state(app_state_t s)
{
    s_state = s;
    ESP_LOGI(TAG, "state: %s", state_name(s));
    if (s == APP_STATE_IDLE)
        display_status_line("Ready", "Hold BOOT to talk");
    else if (s != APP_STATE_WIFI_PROVISIONING && s != APP_STATE_ERROR)
        display_status_line(state_name(s), NULL);
}

void app_post_event(app_event_t ev, const char *text)
{
    app_msg_t m = { .ev = ev, .text = {0} };
    if (text) strlcpy(m.text, text, sizeof(m.text));
    if (s_events) xQueueSend(s_events, &m, 0);
}

/* ---- audio uplink: encoded opus frames -> websocket ---- */
static void on_encoded_frame(const uint8_t *opus, size_t len)
{
    if (s_state == APP_STATE_LISTENING)
        session_send_audio(opus, len);
}

/* ---- BOOT button polling (debounced) ---- */
static bool button_pressed(void)
{
    return gpio_get_level(BOARD_BUTTON_BOOT) == 0;   /* active low */
}

static void button_check(bool *was_pressed)
{
    bool now = button_pressed();
    if (now && !*was_pressed) {
        vTaskDelay(pdMS_TO_TICKS(30));    /* debounce */
        if (button_pressed()) app_post_event(APP_EVENT_BTN_DOWN, NULL);
    } else if (!now && *was_pressed) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (!button_pressed()) app_post_event(APP_EVENT_BTN_UP, NULL);
    }
    *was_pressed = now;
}

/* ---- session lifecycle ---- */
static void open_session(void)
{
    set_state(APP_STATE_CONNECTING);
    if (session_start() != 0) {
        display_status_line("Connect failed", "Hold BOOT to retry");
        set_state(APP_STATE_IDLE);
        return;
    }
    set_state(APP_STATE_LISTENING);
    audio_clear_playback();
    session_start_listening();
    audio_start_mic();
}

static void close_session(void)
{
    audio_stop_mic();
    audio_clear_playback();
    session_stop();
    set_state(APP_STATE_IDLE);
}

static void handle_event(app_msg_t *m)
{
    switch (m->ev) {
    case APP_EVENT_BTN_DOWN:
        if (s_state == APP_STATE_IDLE)
            open_session();
        else if (s_state == APP_STATE_SPEAKING) {
            /* barge-in: abort the current TTS and go back to listening */
            session_send_abort();
            audio_clear_playback();
            session_start_listening();
            audio_start_mic();
            set_state(APP_STATE_LISTENING);
        }
        break;
    case APP_EVENT_BTN_UP:
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            session_stop_listening();
            /* stay connected until the response finishes playing */
            set_state(APP_STATE_SPEAKING);
            if (!audio_is_playing() && !session_is_open()) close_session();
        }
        break;
    case APP_EVENT_STT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_START:
        if (s_state == APP_STATE_LISTENING) audio_stop_mic();
        set_state(APP_STATE_SPEAKING);
        break;
    case APP_EVENT_TTS_TEXT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_STOP:
        set_state(APP_STATE_IDLE);
        /* let the last audio drain, then close the channel */
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_state == APP_STATE_IDLE) close_session();
        break;
    case APP_EVENT_EMOTION:
        display_emotion(m->text);
        break;
    case APP_EVENT_ALERT:
        display_status_line("Alert", m->text);
        break;
    case APP_EVENT_WS_CLOSED:
        if (s_state == APP_STATE_LISTENING || s_state == APP_STATE_SPEAKING)
            close_session();
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    display_init();
    display_status_line("Booting", "XIAO ESP32-S3 voice");
    app_msg_t m;
    s_events = xQueueCreate(16, sizeof(app_msg_t));

    wifi_prov_start();                 /* blocks until station is connected */

    set_state(APP_STATE_ACTIVATING);
    if (ota_fetch(&g_ota_info) != 0) {
        ESP_LOGE(TAG, "OTA fetch failed");
        display_status_line("OTA failed", "Check network / OTA");
        set_state(APP_STATE_ERROR);
        /* keep retrying in the background */
        while (ota_fetch(&g_ota_info) != 0) {
            vTaskDelay(pdMS_TO_TICKS(15000));
        }
    }
    if (g_ota_info.activation_code[0]) {
        /* first activation: show the code until bound at xiaozhi.me */
        ESP_LOGI(TAG, "activation code: %s", g_ota_info.activation_code);
        display_activation(g_ota_info.activation_code);
        /* the server will accept the session anyway once the code is entered;
           retry OTA periodically to pick up the websocket url after binding */
        while (!g_ota_info.websocket_url[0]) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            ota_fetch(&g_ota_info);
            if (g_ota_info.activation_code[0])
                display_activation(g_ota_info.activation_code);
        }
        set_state(APP_STATE_IDLE);
    }

    opus_init();
    if (audio_init(on_encoded_frame) != 0) {
        display_status_line("Audio init failed", NULL);
        set_state(APP_STATE_ERROR);
        vTaskDelay(portMAX_DELAY);
    }

    session_init();
    set_state(APP_STATE_IDLE);

    bool btn = button_pressed();
    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&btn);
    }
}