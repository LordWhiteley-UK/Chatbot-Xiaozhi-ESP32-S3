/**
 * main.c — app entry point and device state machine.
 *
 * Flow: NVS → display → WiFi → OTA activation → audio → wake word → session.
 * The MQTT session stays open; the device waits in IDLE standby for the
 * local wake word ("Computer", esp-sr WakeNet9).  Only then does it arm
 * a listening round.
 *
 * In AUTO listen mode the server detects end-of-speech with its own VAD —
 * the device sends "listen start" with mode "auto" and streams audio until
 * the server responds with TTS.  No on-device VAD, no "listen stop", no
 * conversation timeout.  After TTS_STOP the device goes back to standby
 * (wake word re-arms).  The BOOT button still works as an optional trigger.
 *
 * State transitions:
 *   Idle -> Connecting -> Listening <-> Speaking -> Idle
 */
#include "app.h"
#include "board.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
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
static bool s_wake_ok;
static bool s_pending_listen;   /* wake word fired while session was closed */

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
        display_status_line("Ready", s_wake_ok ? "Say \"Computer\"" : NULL);
    else if (s != APP_STATE_WIFI_PROVISIONING && s != APP_STATE_ERROR)
        display_status_line(state_name(s), NULL);
}

void app_post_event(app_event_t ev, const char *text)
{
    app_msg_t m = { .ev = ev, .text = {0} };
    if (text) strlcpy(m.text, text, sizeof(m.text));
    if (s_events) xQueueSend(s_events, &m, 0);
}

/* uplink: encoded opus frames → session */
static void on_encoded_frame(const uint8_t *opus, size_t len)
{
    if (s_state == APP_STATE_LISTENING)
        session_send_audio(opus, len);
}

/* ── BOOT button (debounced polling) ────────────────────────────── */
static bool button_pressed(void)
{
    return gpio_get_level(BOARD_BUTTON_BOOT) == 0;
}
static void button_check(bool *was)
{
    bool now = button_pressed();
    if (now && !*was) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (button_pressed()) app_post_event(APP_EVENT_BTN_DOWN, NULL);
    } else if (!now && *was) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (!button_pressed()) app_post_event(APP_EVENT_BTN_UP, NULL);
    }
    *was = now;
}

/* ── session lifecycle ──────────────────────────────────────────── */
#define LISTEN_ARM_DELAY_US (1200LL * 1000)   /* skip "Computer" tail */
static int64_t s_listen_arm_at;

static void enter_standby(void)
{
    audio_stop_mic();
    audio_clear_playback();
    session_stop_listening();
    s_listen_arm_at = 0;
    if (s_wake_ok && session_is_open()) {
        set_state(APP_STATE_IDLE);
        wake_word_set_armed(true);
    } else {
        set_state(APP_STATE_IDLE);
    }
}

static void start_listening_round(void)
{
    wake_word_set_armed(false);
    s_listen_arm_at = esp_timer_get_time() + LISTEN_ARM_DELAY_US;
    set_state(APP_STATE_LISTENING);
}

static void open_session(void)
{
    set_state(APP_STATE_CONNECTING);
    if (session_start() != 0) {
        ESP_LOGW(TAG, "session start failed, will retry on wake word");
        set_state(APP_STATE_IDLE);
        if (s_wake_ok) wake_word_set_armed(true);
        s_pending_listen = false;
        return;
    }
    if (s_pending_listen) {
        s_pending_listen = false;
        start_listening_round();
    } else if (s_wake_ok) {
        enter_standby();
    } else {
        /* no wake-word model: listen continuously */
        set_state(APP_STATE_LISTENING);
        audio_clear_playback();
        session_start_listening();
        audio_start_mic();
    }
}

static void close_session(void)
{
    audio_stop_mic();
    audio_clear_playback();
    session_stop();
    s_listen_arm_at = 0;
    set_state(APP_STATE_IDLE);
}

static void handle_event(app_msg_t *m)
{
    switch (m->ev) {
    case APP_EVENT_WAKE_WORD:
        if (s_state == APP_STATE_IDLE) {
            if (session_is_open()) {
                audio_clear_playback();
                start_listening_round();
            } else {
                /* session was closed by server goodbye — reconnect now */
                s_pending_listen = true;
                open_session();
            }
        }
        break;
    case APP_EVENT_BTN_DOWN:
        if (s_state == APP_STATE_IDLE && s_wake_ok) {
            if (session_is_open()) {
                start_listening_round();
            } else {
                s_pending_listen = true;
                open_session();
            }
        } else if (s_state == APP_STATE_SPEAKING) {
            session_send_abort();
            audio_clear_playback();
            start_listening_round();
        }
        break;
    case APP_EVENT_STT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_START:
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            session_stop_listening();
        }
        audio_play_flush();
        set_state(APP_STATE_SPEAKING);
        break;
    case APP_EVENT_TTS_TEXT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_STOP:
        audio_clear_playback();
        if (session_is_open())
            enter_standby();
        else
            set_state(APP_STATE_IDLE);
        break;
    case APP_EVENT_EMOTION:
        display_emotion(m->text);
        break;
    case APP_EVENT_ALERT:
        display_status_line("Alert", m->text);
        break;
    case APP_EVENT_WS_CLOSED:
        /* Server closed the idle session (goodbye).  Don't reconnect in a
           tight loop — go to idle and arm the wake word.  The next wake
           word detection will reconnect on demand. */
        close_session();
        if (s_wake_ok) wake_word_set_armed(true);
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

    wifi_prov_start();                 /* blocks until station connected */

    set_state(APP_STATE_ACTIVATING);
    if (ota_fetch(&g_ota_info) != 0) {
        ESP_LOGE(TAG, "OTA fetch failed");
        display_status_line("OTA failed", "Check network / OTA");
        set_state(APP_STATE_ERROR);
        while (ota_fetch(&g_ota_info) != 0)
            vTaskDelay(pdMS_TO_TICKS(15000));
    }
    if (g_ota_info.activation_code[0]) {
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

    s_wake_ok = wake_word_init() == 0;
    if (s_wake_ok)
        audio_set_pcm_cb(wake_word_feed);

    session_init();
    set_state(APP_STATE_IDLE);
    open_session();

    bool btn = button_pressed();
    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&btn);
        /* arm the listening round after the wake-word tail passes */
        if (s_listen_arm_at && esp_timer_get_time() >= s_listen_arm_at) {
            s_listen_arm_at = 0;
            session_start_listening();
            audio_start_mic();
        }
    }
}