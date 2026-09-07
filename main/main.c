/**
 * main.c — app entry point and device state machine (manual mode).
 *
 * Flow: NVS -> display -> WiFi (provision if needed) -> OTA activation ->
 *       audio -> hands-free session.  There is no user button in this
 *       wiring (every spare GPIO is taken by mic/amp/OLED/USB), so the
 *       session opens automatically and the device listens continuously:
 *       while the assistant's TTS plays, the mic is muted (half-duplex),
 *       and listening resumes as soon as playback finishes.
 *
 * State transitions follow docs/websocket.md §6.4 (manual mode):
 *   Idle -> Connecting -> Listening <-> Speaking
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
        display_status_line("Ready", "Connecting hands-free");
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

/* ---- session lifecycle (hands-free: no button, auto reconnect) ---- */
static void open_session(void)
{
    set_state(APP_STATE_CONNECTING);
    if (session_start() != 0) {
        display_status_line("Connect failed", "Retrying in 5s");
        set_state(APP_STATE_IDLE);
        vTaskDelay(pdMS_TO_TICKS(5000));
        open_session();
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
        /* the on-board BOOT button still works as an optional mute toggle:
           pressing it while the assistant speaks aborts the playback */
        if (s_state == APP_STATE_SPEAKING) {
            session_send_abort();
            audio_clear_playback();
            session_start_listening();
            audio_start_mic();
            set_state(APP_STATE_LISTENING);
        }
        break;
    case APP_EVENT_STT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_START:
        /* half-duplex: mute the mic while the assistant speaks */
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            session_stop_listening();
        }
        set_state(APP_STATE_SPEAKING);
        break;
    case APP_EVENT_TTS_TEXT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_STOP:
        /* resume listening for the next utterance (session stays open) */
        audio_clear_playback();
        if (session_is_open()) {
            session_start_listening();
            audio_start_mic();
            set_state(APP_STATE_LISTENING);
        } else {
            set_state(APP_STATE_IDLE);
        }
        break;
    case APP_EVENT_EMOTION:
        display_emotion(m->text);
        break;
    case APP_EVENT_ALERT:
        display_status_line("Alert", m->text);
        break;
    case APP_EVENT_WS_CLOSED:
        /* reconnect and keep the conversation going */
        close_session();
        vTaskDelay(pdMS_TO_TICKS(2000));
        open_session();
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
        /* first activation: show the code until it is bound at xiaozhi.me.
           The server only completes the session hello for bound devices,
           so poll OTA until the response stops carrying an activation
           code (i.e. the device has been registered). */
        ESP_LOGI(TAG, "activation code: %s", g_ota_info.activation_code);
        display_activation(g_ota_info.activation_code);
        while (g_ota_info.activation_code[0]) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            ota_fetch(&g_ota_info);
            if (g_ota_info.activation_code[0])
                display_activation(g_ota_info.activation_code);
        }
        ESP_LOGI(TAG, "device bound, resuming boot");
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
    open_session();          /* hands-free: connect and listen right away */

    bool btn = button_pressed();
    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&btn);
    }
}