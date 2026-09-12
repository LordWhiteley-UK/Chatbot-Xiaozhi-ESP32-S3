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
 * the server responds with TTS.  No on-device VAD, no "listen stop".
 * After TTS_STOP and playback drain the device enters conversation mode:
 * a fresh listen round for CONV_TIMEOUT_US so follow-up questions need no
 * wake word; on timeout it goes back to standby (wake word re-arms).
 * The buttons work as optional triggers (short press = wake/barge-in,
 * double/triple = volume, long press = face display).
 *
 * Echo-avoidance invariants (all three are always true):
 *   - the wake word is disarmed whenever the speaker may be sounding
 *     (TTS_START → disarm; re-arm only after echo-fade delay + muzzle);
 *   - mic uplink is hardware-gated: mic_task never encodes while the play
 *     ring is non-empty (audio_is_playing() ground truth);
 *   - a listen round that draws no server response within
 *     LISTEN_WATCHDOG_US is abandoned (kills false-wake ambient loops).
 *
 * State transitions:
 *   Idle -> Connecting -> Listening <-> Speaking -> Idle
 */
#include "app.h"
#include "board.h"
#include "face_display.h"

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
static bool s_face_mode;        /* face display toggle (BOOT long-press) */
static bool s_tts_finishing;    /* TTS_STOP received, waiting for playback to drain */
static int64_t s_tts_quiet_at;  /* when audio_is_playing() went false (0 = still playing or not started) */
static int64_t s_tts_deadline;  /* hard timeout for TTS finishing */
static int64_t s_speaking_since; /* watchdog: when we entered SPEAKING */
static int64_t s_speak_quiet_at; /* watchdog: when the ring buffer went empty */
static int64_t s_conv_deadline;  /* conversation mode: when to go to sleep */

/* Volume control: 5 levels, cycled by the external button.
   Short press = quieter, long press = louder. */
static const int vol_levels[] = {20, 40, 60, 80, 100};
static int s_vol_idx = 1;        /* default 40% */

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
    if (!s_face_mode) {
        if (s == APP_STATE_IDLE)
            display_status_line("Ready", s_wake_ok ? "Say \"Computer\"" : NULL);
        else if (s != APP_STATE_WIFI_PROVISIONING && s != APP_STATE_ERROR)
            display_status_line(state_name(s), NULL);
    }
    face_display_set_state(s);
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

/* ── buttons (debounced polling + long-press) ──────────────────── */
/* Two buttons share the same logic: the on-board BOOT (GPIO0) and an
   optional external momentary switch on D2/GPIO3.  Both fire the same
   events: short press = wake/barge-in, long press = toggle face. */
typedef struct {
    int pin;
    int64_t down_at;
    bool armed;
    bool prev;
} button_t;

static button_t s_boot_btn = { .pin = BOARD_BUTTON_BOOT };
static button_t s_ext_btn  = { .pin = BOARD_BUTTON_EXT  };

/* Multi-press detection: 1 press = wake/barge-in, 2 = volume up,
   3 = volume down, long press = face toggle.  Both buttons share the
   counter so the user can use either one. */
static int      s_press_count;
static int64_t  s_press_window_end;

static void button_check(button_t *b)
{
    bool now = gpio_get_level(b->pin) == 0;
    if (now && !b->prev) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(b->pin) == 0) {
            b->down_at = esp_timer_get_time();
            b->armed = true;
        } else {
            now = false;          /* glitch */
        }
    } else if (!now && b->prev) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(b->pin) != 0) {
            if (b->armed) {
                /* short press — add to multi-press counter */
                b->armed = false;
                s_press_count++;
                s_press_window_end = esp_timer_get_time() + 300000; /* 300 ms */
            }
        } else {
            now = true;           /* glitch */
        }
    }
    b->prev = now;
}

/* ── session lifecycle ──────────────────────────────────────────── */
#define LISTEN_ARM_DELAY_US (1200LL * 1000)   /* skip "Computer" tail */
#define WAKE_REARM_DELAY_US  (1500LL * 1000)   /* wait for TTS echo to fade */
#define CONV_TIMEOUT_US      (10LL * 1000000)  /* conversation mode: stay awake this long after TTS */
#define LISTEN_ARM_FAST_US   (300LL * 1000)    /* button/conversation: no wake-word tail to skip */
static int64_t s_listen_arm_at;
static int64_t s_wake_rearm_at;
static int64_t s_listen_since;   /* when the current LISTENING round started */

/* If a wake-triggered listen round gets NO server response (no STT, no TTS)
   within this window, drop back to standby.  Without this, a false wake-word
   trigger (TV/room noise) leaves the mic hot forever and every stray sound
   is transcribed and answered — which sounds like an endless echo loop.
   Conversation-mode follow-up listens are bounded separately by
   CONV_TIMEOUT_US; this covers the initial wake round only. */
#define LISTEN_WATCHDOG_US (45LL * 1000000)

static void enter_standby(void)
{
    audio_stop_mic();
    audio_clear_playback();
    session_stop_listening();
    s_listen_arm_at = 0;
    s_conv_deadline = 0;
    if (s_wake_ok && session_is_open()) {
        set_state(APP_STATE_IDLE);
        /* Delay arming the wake word so the mic doesn't trigger on the
           tail-end TTS audio still coming from the speaker + room echo. */
        s_wake_rearm_at = esp_timer_get_time() + WAKE_REARM_DELAY_US;
    } else {
        set_state(APP_STATE_IDLE);
    }
}

static void start_listening_round(int64_t arm_delay_us)
{
    wake_word_set_armed(false);
    /* Always clear the session's listen flag before arming a NEW round, so
       the deferred session_start_listening() actually transmits "listen
       start".  Its early-return guard (`if (s_listening) return`) otherwise
       silently swallows the start after a barge-in abort: the TTS_START path
       never clears the flag (AUTO mode), and finish_tts_round — which clears
       it — is skipped on the interrupt path.  Symptom: device shows
       Listening and streams uplink audio for minutes, server never sends a
       listen start acknowledgement or STT, device appears dead.  Clearing
       the flag costs nothing in AUTO mode (local flag only, no MQTT send). */
    session_stop_listening();
    s_listen_arm_at = esp_timer_get_time() + arm_delay_us;
    s_listen_since = esp_timer_get_time();
    set_state(APP_STATE_LISTENING);
}

/* Called when a TTS answer has finished playing (TTS_STOP received and
   audio drained, or the speaking watchdog fired).  If the session is
   still open, enter "conversation mode" — start a new listening round
   immediately so the user can ask follow-up questions without the wake
   word.  A 10-second timeout returns to sleep if no follow-up comes. */
static void finish_tts_round(void)
{
    audio_stop_accept();
    audio_clear_playback();
    session_stop_listening();   /* clear s_listening so the next start
                                   actually sends "listen start" to server */
    s_tts_finishing = false;
    s_tts_quiet_at = 0;
    s_speak_quiet_at = 0;
    if (session_is_open()) {
        ESP_LOGI(TAG, "conversation mode — listening for follow-up");
        start_listening_round(LISTEN_ARM_FAST_US);   /* no wake tail here; don't clip early speech */
        s_conv_deadline = esp_timer_get_time() + CONV_TIMEOUT_US;
    } else {
        set_state(APP_STATE_IDLE);
        if (s_wake_ok)
            s_wake_rearm_at = esp_timer_get_time() + WAKE_REARM_DELAY_US;
    }
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
        /* session setup took seconds — any wake-word tail is long gone */
        start_listening_round(LISTEN_ARM_FAST_US);
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

static void handle_event(app_msg_t *m)
{
    switch (m->ev) {
    case APP_EVENT_WAKE_WORD:
        if (s_state == APP_STATE_IDLE) {
            if (session_is_open()) {
                audio_clear_playback();
                start_listening_round(LISTEN_ARM_DELAY_US);  /* skip "Computer" tail */
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
                start_listening_round(LISTEN_ARM_FAST_US);  /* button press: no wake tail */
            } else {
                s_pending_listen = true;
                open_session();
            }
        } else if (s_state == APP_STATE_SPEAKING || s_tts_finishing) {
            /* Barge-in mid-answer. Use the LONG arm delay, not the fast one:
               playback was sounding right up until this press — the I2S DMA
               tail and the room echo of the truncated answer need ~1 s to
               fade before the mic opens, or the server hears the tail of our
               own TTS, transcribes it as new speech, and we get an echo
               loop.  (Conversation follow-ups can arm fast because playback
               there has already been silent for 3 s before finish_tts_round
               runs.) */
            s_tts_finishing = false;
            session_send_abort();
            audio_stop_mic();
            audio_clear_playback();
            start_listening_round(LISTEN_ARM_DELAY_US);  /* long delay: let TTS echo fade */
        }
        break;
    case APP_EVENT_STT:
        if (!s_face_mode) display_text(m->text);
        break;
    case APP_EVENT_TTS_START:
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            /* AUTO mode: the server already owns the listen state.  Sending
               an extra "listen stop" here correlated with the server ending
               the whole session ("goodbye") right after each reply. */
        }
        /* Don't let our own TTS output echo into a phantom wake trigger
           while we're speaking/finishing; the standby path re-arms. */
        wake_word_set_armed(false);
        s_listen_arm_at = 0;        /* cancel any pending mic-arm — we're speaking now */
        s_tts_finishing = false;    /* a new response supersedes any prior drain */
        s_tts_quiet_at = 0;
        s_conv_deadline = 0;        /* cancel conversation timeout — new answer */
        s_speaking_since = esp_timer_get_time();
        audio_play_flush();
        set_state(APP_STATE_SPEAKING);
        break;
    case APP_EVENT_TTS_TEXT:
        if (!s_face_mode) display_text(m->text);
        break;
    case APP_EVENT_TTS_STOP:
        /* Only process if we're actually speaking — a stale TTS_STOP from
           an aborted round (barge-in) must NOT trigger the finishing path
           or it kills the new listening round and re-arms the wake word
           on TTS echo, causing the voice loop. */
        if (s_state != APP_STATE_SPEAKING && !s_tts_finishing) {
            ESP_LOGI(TAG, "tts stop (stale — not speaking, ignoring)");
            break;
        }
        ESP_LOGI(TAG, "tts stop received — waiting for audio to drain");
        s_tts_finishing = true;
        s_tts_quiet_at = esp_timer_get_time();
        s_tts_deadline = esp_timer_get_time() + 30000000;
        break;
    case APP_EVENT_EMOTION:
        if (!s_face_mode) display_emotion(m->text);
        break;
    case APP_EVENT_BTN_LONG:
        /* Toggle face display mode (button held > 800 ms).
           The short-press BTN_DOWN is suppressed (armed flag cleared in
           the main loop before posting this) so only this event fires.
           Clean up any active listening/connecting before switching modes;
           speaking and TTS-finishing states are left alone so audio keeps
           playing. */
        if (s_state == APP_STATE_LISTENING) {
            audio_stop_mic();
            session_stop_listening();
            enter_standby();
        } else if (s_state == APP_STATE_CONNECTING) {
            /* no audio can be playing yet in CONNECTING, so a plain
               session_stop() is safe here */
            session_stop();
            set_state(APP_STATE_IDLE);
            if (s_wake_ok) wake_word_set_armed(true);
        }
        s_face_mode = !s_face_mode;
        face_display_set_enabled(s_face_mode);
        if (s_face_mode)
            face_display_set_state(s_state);
        else
            set_state(s_state);       /* refresh info display */
        break;
    case APP_EVENT_ALERT:
        if (!s_face_mode) display_status_line("Alert", m->text);
        break;
    case APP_EVENT_VOL_DOWN:
        if (s_vol_idx > 0) s_vol_idx--;
        audio_set_volume(vol_levels[s_vol_idx]);
        ESP_LOGI(TAG, "volume -> %d%%", vol_levels[s_vol_idx]);
        if (!s_face_mode) {
            char buf[16];
            snprintf(buf, sizeof(buf), "Volume %d%%", vol_levels[s_vol_idx]);
            display_status_line("Volume", buf);
        }
        break;
    case APP_EVENT_VOL_UP:
        if (s_vol_idx < 4) s_vol_idx++;
        audio_set_volume(vol_levels[s_vol_idx]);
        ESP_LOGI(TAG, "volume -> %d%%", vol_levels[s_vol_idx]);
        if (!s_face_mode) {
            char buf[16];
            snprintf(buf, sizeof(buf), "Volume %d%%", vol_levels[s_vol_idx]);
            display_status_line("Volume", buf);
        }
        break;
    case APP_EVENT_WS_CLOSED:
        /* Server closed the session ("goodbye") — often while the TTS
           answer is still playing out of the speaker.  Two rules here:
           1. NEVER clear playback — let the queued answer finish or the
              user hears the reply truncated mid-sentence.
           2. NEVER arm the wake word while audio could still be sounding —
              the room echo of our own TTS output would be re-detected as
              "Computer", reconnect, get transcribed as phantom speech
              ("Yeah."), answered, closed again... an infinite voice loop.
           If the answer is mid-flight, route through the TTS-finishing
           drain (3 s of real quiet + echo-fade delay before re-arm);
           otherwise idle straight away. */
        ESP_LOGW(TAG, "server closed session%s", audio_is_playing() ? " — draining playback" : "");
        audio_stop_mic();
        s_listen_arm_at = 0;
        s_conv_deadline = 0;        /* no conversation after session closed */
        audio_stop_accept();     /* no NEW packets; existing audio plays out */
        session_stop();          /* release socket/crypto resources (no clear_playback!) */
        if (audio_is_playing()) {
            s_tts_finishing = true;
            s_tts_quiet_at = 0;                 /* quiet timer starts when ring empties */
            s_tts_deadline = esp_timer_get_time() + 30000000;
        } else {
            s_tts_finishing = false;
            s_tts_quiet_at = 0;
            set_state(APP_STATE_IDLE);
            if (s_wake_ok)
                s_wake_rearm_at = esp_timer_get_time() + WAKE_REARM_DELAY_US;
        }
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

    audio_set_volume(vol_levels[s_vol_idx]);   /* apply default volume */

    face_display_init();
    session_init();
    set_state(APP_STATE_IDLE);
    open_session();

    /* Configure external button pin with internal pull-up */
    gpio_config_t ext_btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_EXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ext_btn_cfg);

    s_boot_btn.prev = gpio_get_level(BOARD_BUTTON_BOOT) == 0;
    s_ext_btn.prev  = gpio_get_level(BOARD_BUTTON_EXT) == 0;

    while (true) {
        if (xQueueReceive(s_events, &m, pdMS_TO_TICKS(20)))
            handle_event(&m);
        button_check(&s_boot_btn);
        button_check(&s_ext_btn);
        /* long-press detection: if held > 800 ms, fire BTN_LONG */
        if (s_boot_btn.armed && gpio_get_level(BOARD_BUTTON_BOOT) == 0 &&
            esp_timer_get_time() - s_boot_btn.down_at > 800000) {
            s_boot_btn.armed = false;
            s_press_count = 0;
            s_press_window_end = 0;
            app_post_event(APP_EVENT_BTN_LONG, NULL);
        }
        if (s_ext_btn.armed && gpio_get_level(BOARD_BUTTON_EXT) == 0 &&
            esp_timer_get_time() - s_ext_btn.down_at > 800000) {
            s_ext_btn.armed = false;
            s_press_count = 0;
            s_press_window_end = 0;
            app_post_event(APP_EVENT_BTN_LONG, NULL);
        }
        /* multi-press dispatch: when the 300 ms window expires, fire
           the event matching the press count. */
        if (s_press_window_end && esp_timer_get_time() >= s_press_window_end) {
            s_press_window_end = 0;
            app_event_t ev;
            switch (s_press_count) {
            case 2:  ev = APP_EVENT_VOL_UP;   break;
            case 3:  ev = APP_EVENT_VOL_DOWN; break;
            default: ev = APP_EVENT_BTN_DOWN; break;  /* 1 or 4+ = wake */
            }
            s_press_count = 0;
            app_post_event(ev, NULL);
        }
        /* arm the listening round after the wake-word tail passes */
        if (s_listen_arm_at && esp_timer_get_time() >= s_listen_arm_at) {
            s_listen_arm_at = 0;
            /* Only start the mic if we're still LISTENING.  If a TTS_START
               arrived during the 1.2 s arm delay the state is now SPEAKING
               and arming the mic here would leave s_mic_running stale (and
               skip the uplink warm-up) on the next conversation round. */
            if (s_state == APP_STATE_LISTENING) {
                session_start_listening();
                audio_start_mic();
            }
        }
        /* rearm wake word after TTS echo fade-out */
        if (s_wake_rearm_at && esp_timer_get_time() >= s_wake_rearm_at) {
            s_wake_rearm_at = 0;
            wake_word_set_armed(true);
        }
        int64_t now = esp_timer_get_time();
        /* SPEAKING watchdog: the server sometimes never sends TTS_STOP
           (observed in logs: playback stalls, state stays SPEAKING forever
           and the wake word is disarmed — device appears completely dead
           until power-cycled).  If the ring buffer has been empty for 10 s
           while in SPEAKING without a TTS_STOP, treat the round as over. */
        if (s_state == APP_STATE_SPEAKING && !s_tts_finishing &&
            now >= s_speaking_since + 5000000 &&   /* grace: let first UDP arrive */
            !audio_is_playing()) {
            if (!s_speak_quiet_at)
                s_speak_quiet_at = now;
            if (now - s_speak_quiet_at > 10000000) {
                ESP_LOGW(TAG, "no TTS_STOP after playback silent — finishing round");
                s_speak_quiet_at = 0;
                finish_tts_round();
            }
        } else {
            s_speak_quiet_at = 0;
        }
        /* TTS finishing: wait for playback to drain, then enter standby.
           The quiet timer resets whenever audio is still playing; after 3 s
           of silence (or a 30 s hard timeout) we transition to standby. */
        if (s_tts_finishing) {
            if (audio_is_playing())
                s_tts_quiet_at = 0;        /* still playing — reset quiet */
            else if (s_tts_quiet_at == 0)
                s_tts_quiet_at = now;       /* just went quiet */
            if (now >= s_tts_deadline ||
                (s_tts_quiet_at && now - s_tts_quiet_at > 3000000)) {
                finish_tts_round();
            }
        }
        /* conversation mode timeout: if no follow-up came, go to sleep */
        if (s_conv_deadline && now >= s_conv_deadline) {
            s_conv_deadline = 0;
            ESP_LOGI(TAG, "conversation timeout — going to sleep");
            enter_standby();
        }
        /* listen watchdog: a wake-triggered round that gets no server
           response at all (false wake on room noise) would otherwise stay
           hot-mic indefinitely and keep answering ambient sounds. */
        if (s_state == APP_STATE_LISTENING && !s_conv_deadline &&
            s_listen_since && now - s_listen_since > LISTEN_WATCHDOG_US) {
            ESP_LOGW(TAG, "listen timeout — no server response, back to standby");
            enter_standby();
        }
    }
}