/**
 * main.c — app entry point and device state machine (wake-word standby).
 *
 * Flow: NVS -> display -> WiFi (provision if needed) -> OTA activation ->
 *       audio -> wake word -> session.  The MQTT session stays open and
 *       the device waits in IDLE standby for the local wake word
 *       ("Computer", esp-sr wakenet): only then does it arm a listening
 *       round.  Rounds use manual listen mode: 1.2 s after the wake word
 *       (to skip the "Computer" tail) the device sends "listen start" and
 *       then opens the mic uplink; when the on-device VAD detects the end
 *       of the utterance it sends "listen stop" (in manual mode the server
 *       finalizes ASR only on stop — without it the round deadlocks and no
 *       TTS ever returns).  While the assistant's TTS plays the mic uplink
 *       is muted (half-duplex, no AEC).  The BOOT button still works as an
 *       optional trigger / barge-in.
 *
 * State transitions follow docs/websocket.md §6.4 (manual mode):
 *   Idle -> Connecting -> Listening <-> Speaking -> Idle (standby)
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
static bool s_wake_ok;      /* wake-word engine ready (model loaded) */

/* After a wake word the device stays in conversation (auto-relisten after
   each reply) until this deadline passes without activity; then standby. */
#define CONV_TIMEOUT_US (30LL * 1000000)
static int64_t s_conv_deadline;
static void extend_conversation(void)
{
    s_conv_deadline = esp_timer_get_time() + CONV_TIMEOUT_US;
}
static bool in_conversation(void)
{
    return esp_timer_get_time() < s_conv_deadline;
}

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

/* audio.c's VAD fired (mic task context): end of the user's utterance */
static void on_utter_end(void)
{
    app_post_event(APP_EVENT_UTTER_END, NULL);
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

/* Arming a round: the wake-word utterance ("Computer") outlives detection
   by a few hundred ms, so the round is held for LISTEN_ARM_DELAY_US first
   — only then is "listen start" sent and the mic uplink opened, in that
   order (spec §6.2: SendStartListening, then mic streaming begins). */
#define LISTEN_ARM_DELAY_US (1200LL * 1000)
static int64_t s_listen_arm_at;

/* After a "listen stop" (VAD end-of-speech) the server normally answers
   with STT+TTS within seconds; if nothing comes back, re-open the round
   (inside the conversation window) instead of hanging forever. */
#define REPLY_TIMEOUT_US (15LL * 1000000)
static int64_t s_reply_deadline;

static void enter_standby(void)
{
    audio_stop_mic();              /* uplink muted; mic keeps feeding KWS */
    audio_clear_playback();
    session_stop_listening();      /* close any dangling round cleanly */
    s_listen_arm_at = 0;
    s_reply_deadline = 0;
    if (s_wake_ok && session_is_open()) {
        set_state(APP_STATE_IDLE); /* standby: waiting for the wake word */
        wake_word_set_armed(true);
    } else {
        set_state(APP_STATE_IDLE);
    }
}

static void start_listening_round(void)
{
    wake_word_set_armed(false);
    s_reply_deadline = 0;
    s_listen_arm_at = esp_timer_get_time() + LISTEN_ARM_DELAY_US;
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
            extend_conversation();
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
        extend_conversation();
        break;
    case APP_EVENT_TTS_START:
        /* half-duplex: mute the mic uplink while the assistant speaks */
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            session_stop_listening();
        }
        s_reply_deadline = 0;
        extend_conversation();
        set_state(APP_STATE_SPEAKING);
        break;
    case APP_EVENT_UTTER_END:
        /* device VAD: the user finished speaking.  Manual mode: the server
           finalizes ASR only on "listen stop" — send it, mute the uplink,
           and wait for the reply (watchdog in the main loop re-listens if
           nothing comes back). */
        if (s_state == APP_STATE_LISTENING) {
            session_stop_listening();
            audio_stop_mic();
            s_reply_deadline = esp_timer_get_time() + REPLY_TIMEOUT_US;
            display_status_line("Thinking", NULL);
        }
        break;
    case APP_EVENT_TTS_TEXT:
        display_text(m->text);
        break;
    case APP_EVENT_TTS_STOP:
        /* within the conversation window, listen straight through for the
           follow-up; only the wake word re-arms an expired conversation */
        if (session_is_open()) {
            if (in_conversation()) {
                start_listening_round();
            } else {
                enter_standby();
            }
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

    /* local "Computer" wake word: mic PCM is fed to it from the mic task */
    s_wake_ok = wake_word_init() == 0;
    if (s_wake_ok)
        audio_set_pcm_cb(wake_word_feed);
    audio_set_utter_end_cb(on_utter_end);   /* VAD end-of-speech -> listen stop */

    session_init();
    set_state(APP_STATE_IDLE);
    open_session();          /* connect; standby until the wake word */

    bool btn = button_pressed();
    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&btn);
        if (s_listen_arm_at && esp_timer_get_time() >= s_listen_arm_at) {
            s_listen_arm_at = 0;
            /* spec order: send "listen start" first, then open the uplink
               (audio.c warmup keeps the JSON publish ahead of the audio) */
            session_start_listening();
            audio_start_mic();
        }
        /* reply watchdog: round ended (listen stop sent) but nothing came back */
        if (s_reply_deadline && esp_timer_get_time() >= s_reply_deadline) {
            s_reply_deadline = 0;
            if (s_state == APP_STATE_LISTENING && session_is_open()) {
                ESP_LOGI(TAG, "no server reply in %d s; %s",
                         (int)(REPLY_TIMEOUT_US / 1000000),
                         in_conversation() ? "re-listening" : "back to standby");
                if (in_conversation())
                    start_listening_round();
                else
                    enter_standby();
            }
        }
        /* listening round that never heard anything -> back to standby */
        if (s_state == APP_STATE_LISTENING && !s_listen_arm_at &&
            !s_reply_deadline && !in_conversation() && session_is_open())
            enter_standby();
    }
}