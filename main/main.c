/**
 * main.c — app entry point and device state machine (wake-word standby).
 *
 * Flow: NVS -> display -> WiFi (provision if needed) -> OTA activation ->
 *       audio -> wake word -> session.  The MQTT session stays open and
 *       the device waits in IDLE standby for the local wake word
 *       ("Computer", esp-sr wakenet): only then does it arm a listening
 *       round.  While the assistant's TTS plays the mic uplink is muted
 *       (half-duplex, no AEC), and after playback the device returns to
 *       standby instead of listening again.  The BOOT button still works
 *       as an optional trigger.
 *
 * State transitions follow docs/websocket.md §6.4 (manual mode):
 *   Idle -> Connecting -> Listening <-> Speaking -> Idle (standby)
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
static bool s_wake_ok;      /* wake-word engine ready (model loaded) */

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

/* ---- session lifecycle (wake-word standby, auto reconnect) ---- */
static void enter_standby(void)
{
    audio_stop_mic();              /* uplink muted; mic keeps feeding KWS */
    audio_clear_playback();
    if (s_wake_ok && session_is_open()) {
        set_state(APP_STATE_IDLE); /* standby: waiting for the wake word */
        wake_word_set_armed(true);
    } else {
        set_state(APP_STATE_IDLE);
    }
}

static void start_listening_round(void)
{
    session_start_listening();
    audio_start_mic();
    wake_word_set_armed(false);
    set_state(APP_STATE_LISTENING);
}

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
    if (s_wake_ok) {
        enter_standby();
    } else {
        /* fallback: no wake-word model, listen continuously as before */
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
    set_state(APP_STATE_IDLE);
}

static void handle_event(app_msg_t *m)
{
    switch (m->ev) {
    case APP_EVENT_WAKE_WORD:
        /* "Computer": open a listening round (or grab it back mid-speech) */
        if (s_state == APP_STATE_IDLE && session_is_open()) {
            audio_clear_playback();
            start_listening_round();
        }
        break;
    case APP_EVENT_BTN_DOWN:
        /* the on-board BOOT button still works as an optional trigger */
        if (s_state == APP_STATE_IDLE && s_wake_ok && session_is_open()) {
            start_listening_round();
        } else if (s_state == APP_STATE_SPEAKING) {
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
        /* half-duplex: mute the mic uplink while the assistant speaks */
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
        /* back to standby: the next round needs the wake word again
           (the session itself stays open) */
        if (session_is_open()) enter_standby();
        else set_state(APP_STATE_IDLE);
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

    /* local "Computer" wake word: mic PCM is fed to it from the mic task */
    s_wake_ok = wake_word_init() == 0;
    if (s_wake_ok)
        audio_set_pcm_cb(wake_word_feed);

    session_init();
    set_state(APP_STATE_IDLE);
    open_session();          /* connect; standby until the wake word */

    bool btn = button_pressed();
    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&btn);
    }
}