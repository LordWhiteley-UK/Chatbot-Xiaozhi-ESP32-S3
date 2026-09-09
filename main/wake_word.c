/**
 * wake_word.c — local "Computer" keyword spotting via esp-sr WakeNet9.
 *
 * The model (CONFIG_SR_WN_WN9_COMPUTER_TTS) is packed into the "model"
 * partition by the esp-sr build (build/srmodels/srmodels.bin) and mmap'd
 * from flash at init. The mic task feeds every captured frame here; the
 * engine is stepped in its fixed chunk size (16-bit samples @16 kHz).
 *
 * Interpretation note (flagged per project ground rules): the spec docs
 * describe a server-side "小智" gate only and don't cover local wake words;
 * "Computer" is the wake word of the official English firmware build the
 * user asked to match.
 */
#include "wake_word.h"
#include "app.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"

static const char *TAG = "wake_word";

static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_iface;
static model_iface_data_t *s_wn;
static char s_word[MODEL_NAME_MAX_LENGTH];
static int s_chunk;                 /* 16-bit samples per detect() call */
static volatile bool s_armed;
static bool s_ready;
static int64_t s_muzzle_until;      /* discard audio until this time (echo guard) */

static int16_t s_buf[512 * 16];     /* pending samples between detect steps */
static int s_pending;

int wake_word_init(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num == 0) {
        ESP_LOGE(TAG, "no models in 'model' partition");
        return -1;
    }
    for (int i = 0; i < s_models->num; i++) {
        const char *name = s_models->model_name[i];
        if (name && strstr(name, ESP_WN_PREFIX)) {
            ESP_LOGI(TAG, "model '%s' (%s)", name, s_models->model_info[i]);
            s_iface = esp_wn_handle_from_name(name);
            s_wn = s_iface->create(name, DET_MODE_90);
            strlcpy(s_word, esp_wn_wakeword_from_name(name), sizeof(s_word));
            break;
        }
    }
    if (!s_wn) {
        ESP_LOGE(TAG, "no wakenet model found");
        return -1;
    }
    s_chunk = s_iface->get_samp_chunksize(s_wn);
    if (s_chunk > (int)sizeof(s_buf) / (int)sizeof(s_buf[0]))
        s_chunk = sizeof(s_buf) / sizeof(s_buf[0]);
    s_ready = true;
    ESP_LOGI(TAG, "wake word '%s' ready, chunk=%d samples @%d Hz",
             s_word, s_chunk, s_iface->get_samp_rate(s_wn));
    return 0;
}

bool wake_word_ready(void)   { return s_ready; }
const char *wake_word_name(void) { return s_word; }
void wake_word_set_armed(bool armed)
{
    s_armed = armed;
    if (armed) {
        /* Flush the partial-sample buffer so the model doesn't process
           stale audio from before the disarm period (e.g. TTS echo). */
        s_pending = 0;
        /* Muzzle: discard audio for 600 ms after arming so the room echo
           of the device's own TTS output doesn't re-trigger the wake word. */
        s_muzzle_until = esp_timer_get_time() + 600000;
    }
}

void wake_word_feed(const int16_t *pcm, int nsamples)
{
    if (!s_ready || !s_armed)
        return;
    /* Muzzle period: discard audio for a short time after arming to
       prevent the wake word from triggering on TTS room echo. */
    if (esp_timer_get_time() < s_muzzle_until)
        return;

    /* step the engine in chunk-size units, buffering the frame remainder */
    while (nsamples > 0) {
        int take = s_chunk - s_pending;
        if (take > nsamples) take = nsamples;
        memcpy(s_buf + s_pending, pcm, take * sizeof(int16_t));
        s_pending += take;
        pcm += take;
        nsamples -= take;
        if (s_pending < s_chunk)
            break;
        wakenet_state_t st = s_iface->detect(s_wn, s_buf);
        s_pending = 0;
        if (st == WAKENET_DETECTED) {
            s_armed = false;                 /* one shot; app re-arms */
            ESP_LOGI(TAG, "wake word '%s' detected", s_word);
            app_post_event(APP_EVENT_WAKE_WORD, NULL);
            break;
        }
    }
}