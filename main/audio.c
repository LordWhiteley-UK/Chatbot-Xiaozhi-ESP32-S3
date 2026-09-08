/**
 * audio.c — INMP441 mic capture and MAX98357A playback over I2S.
 *
 * RX (mic) and TX (amp) run on separate I2S controllers (I2S0 / I2S1)
 * so simultaneous listen/playback works without an external codec IC.
 *
 * Mic:  16 kHz, 32-bit stereo slots (INMP441 data is 24-bit, MSB-aligned),
 *       L/R pin tied to GND → left slot extracted to mono.
 * Amp:  sample rate set dynamically from the server's downlink params.
 *
 * Downlink path (TTS):
 *   UDP rx task → audio_play() → [Opus packet queue] → opus_decode_task
 *     → [PCM ring buffer] → play_task → I2S
 *
 * The Opus decode runs in its own task with a 24 KB stack (the SILK
 * decoder at 24 kHz can use ~8 KB).  The queue (depth 20 = 1.2 s of
 * buffered audio) decouples UDP receive from decode.
 *
 * No on-device VAD: in AUTO listen mode the server detects end-of-speech.
 * The mic task still feeds the wake-word engine continuously; it just
 * doesn't run WebRTC VAD or fire utter-end callbacks.
 */
#include "app.h"
#include "board.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "opus_codec.h"

static const char *TAG = "audio";

#define SAMPLE_RATE_IN   16000
#define FRAME_MS         CONFIG_OPUS_FRAME_DURATION_MS
#define SAMPLES_PER_FRAME (SAMPLE_RATE_IN * FRAME_MS / 1000)   /* 960 @60ms */
#define BYTES_PER_FRAME  (SAMPLES_PER_FRAME * 2)

#define PLAY_RING_SIZE   (48 * 1024)
#define UPLINK_WARMUP_US (200 * 1000)

/* Opus packet queue: 20 packets × 60 ms = 1.2 s jitter buffer */
#define MAX_OPUS_PKT     1400
#define OPUS_QUEUE_LEN   20

typedef struct {
    int len;
    int sample_rate;
    uint8_t data[MAX_OPUS_PKT];
} opus_pkt_t;

static i2s_chan_handle_t s_rx_chan, s_tx_chan;
static int32_t s_i2s_raw[SAMPLES_PER_FRAME * 2];
static int16_t s_pcm_in[SAMPLES_PER_FRAME];

static audio_frame_cb_t s_on_encoded;
static audio_pcm_cb_t s_on_pcm;
static audio_utter_end_cb_t s_on_utter_end;   /* unused in AUTO mode */
static RingbufHandle_t s_play_ring;
static QueueHandle_t s_opus_queue;
static TaskHandle_t s_mic_task, s_play_task, s_decode_task;
static volatile bool s_mic_running;
static volatile int s_play_sample_rate = 16000;
static volatile size_t s_play_pending;
static int64_t s_mic_opened_at;

void audio_start_mic(void)
{
    s_mic_opened_at = esp_timer_get_time();
    s_mic_running = true;
}
void audio_stop_mic(void)  { s_mic_running = false; }
void audio_set_pcm_cb(audio_pcm_cb_t cb) { s_on_pcm = cb; }
void audio_set_utter_end_cb(audio_utter_end_cb_t cb) { s_on_utter_end = cb; }
bool audio_is_playing(void){ return s_play_pending > 0; }

void audio_clear_playback(void)
{
    if (s_play_ring) vRingbufferReset(s_play_ring);
    s_play_pending = 0;
}

void audio_play_flush(void)
{
    if (s_opus_queue) xQueueReset(s_opus_queue);
    if (s_play_ring) vRingbufferReset(s_play_ring);
    s_play_pending = 0;
    opus_decoder_reset();
    ESP_LOGI(TAG, "playback flushed");
}

void audio_set_playback_rate(int sample_rate)
{
    if (sample_rate <= 0 || sample_rate == s_play_sample_rate) return;
    /* Reconfigure the I2S TX clock BEFORE any audio arrives — at hello time.
       This avoids the race condition of disabling/enabling the channel while
       the play_task is in the middle of an i2s_channel_write call. */
    i2s_channel_disable(s_tx_chan);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(s_tx_chan, &clk);
    i2s_channel_enable(s_tx_chan);
    s_play_sample_rate = sample_rate;
    ESP_LOGI(TAG, "playback sample rate -> %d Hz", sample_rate);
}

void audio_play(const uint8_t *opus, size_t len, int sample_rate)
{
    if (!s_opus_queue || len == 0 || len > MAX_OPUS_PKT) return;
    opus_pkt_t pkt = { .len = (int)len, .sample_rate = sample_rate };
    memcpy(pkt.data, opus, len);
    if (xQueueSend(s_opus_queue, &pkt, 0) != pdTRUE) {
        static int s_drops;
        if (++s_drops % 50 == 1)
            ESP_LOGW(TAG, "opus queue full, dropping TTS frame (%d drops)", s_drops);
    }
}

/* ── decode task: Opus queue → PCM ring ─────────────────────────── */
static void opus_decode_task(void *arg)
{
    opus_pkt_t pkt;
    static int16_t pcm[2880 * 2];     /* 24 kHz × 60 ms × 2 headroom */
    int play_frames = 0;

    while (true) {
        if (xQueueReceive(s_opus_queue, &pkt, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        if (pkt.sample_rate > 0 && pkt.sample_rate != s_play_sample_rate) {
            ESP_LOGW(TAG, "sample rate mismatch: pkt=%d, current=%d (expected at hello)",
                     pkt.sample_rate, s_play_sample_rate);
        }

        int nsamples = opus_decode_frame(pkt.data, (size_t)pkt.len,
                                         pcm, sizeof(pcm) / 2);
        if (nsamples <= 0) {
            ESP_LOGW(TAG, "opus decode failed (%d bytes)", pkt.len);
            continue;
        }
        if (xRingbufferSend(s_play_ring, pcm, nsamples * 2,
                            pdMS_TO_TICKS(100)) != pdTRUE)
            ESP_LOGW(TAG, "play ring full, dropping %d samples", nsamples);
        else
            s_play_pending += nsamples * 2;

        if (++play_frames % 20 == 0)
            ESP_LOGI(TAG, "playback: %d frames, %d samples pending, ring free %uB",
                     play_frames, (int)(s_play_pending / 2),
                     (unsigned)xRingbufferGetCurFreeSize(s_play_ring));
    }
}

/* ── mic task: I2S → PCM → [wake word] → opus → callback ────────── */
static void mic_task(void *arg)
{
    size_t bytes_read;
    while (true) {
        if (i2s_channel_read(s_rx_chan, s_i2s_raw, sizeof(s_i2s_raw),
                             &bytes_read, pdMS_TO_TICKS(200)) != ESP_OK) continue;
        int frames = bytes_read / (2 * sizeof(int32_t));
        for (int i = 0; i < frames; i++)
            s_pcm_in[i] = (int16_t)(s_i2s_raw[2 * i] >> 16);

        if (s_on_pcm) s_on_pcm(s_pcm_in, frames);

        if (!s_mic_running) continue;
        if (esp_timer_get_time() - s_mic_opened_at < UPLINK_WARMUP_US) continue;

        size_t enc_len = 0;
        const uint8_t *enc = opus_encode_frame(s_pcm_in, frames, &enc_len);
        if (enc && enc_len > 0 && s_on_encoded) s_on_encoded(enc, enc_len);
    }
}

/* ── playback task: ring → I2S ──────────────────────────────────── */
static void play_task(void *arg)
{
    size_t br;
    int16_t chunk[512];
    i2s_channel_enable(s_tx_chan);
    while (true) {
        void *data = xRingbufferReceiveUpTo(s_play_ring, &br,
                                            pdMS_TO_TICKS(200), sizeof(chunk));
        if (!data) continue;
        memcpy(chunk, data, br);
        vRingbufferReturnItem(s_play_ring, data);
        s_play_pending -= br;
        size_t written = 0;
        while (written < br) {
            size_t w = 0;
            if (i2s_channel_write(s_tx_chan, (uint8_t *)chunk + written,
                                  br - written, &w,
                                  pdMS_TO_TICKS(200)) != ESP_OK) {
                /* channel might be disabled during sample-rate reconfig —
                   give it a moment before retrying the next chunk */
                vTaskDelay(pdMS_TO_TICKS(5));
                break;
            }
            written += w;
        }
    }
}

/* ── init ───────────────────────────────────────────────────────── */
int audio_init(audio_frame_cb_t on_encoded_frame)
{
    s_on_encoded = on_encoded_frame;

    /* RX: INMP441 — 16 kHz, 32-bit stereo slots, left slot = data */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_cfg.dma_desc_num = 8;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx_chan));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_IN),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_MIC_SCK,
            .ws   = BOARD_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_std));
    i2s_channel_enable(s_rx_chan);

    /* TX: MAX98357A — standard Philips, 16-bit mono */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_cfg.dma_desc_num = 8;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx_chan, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_play_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_AMP_BCLK,
            .ws   = BOARD_AMP_LRC,
            .dout = BOARD_AMP_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_std));

    s_play_ring = xRingbufferCreate(PLAY_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_play_ring) { ESP_LOGE(TAG, "play ring alloc failed"); return -1; }

    s_opus_queue = xQueueCreate(OPUS_QUEUE_LEN, sizeof(opus_pkt_t));
    if (!s_opus_queue) { ESP_LOGE(TAG, "opus queue alloc failed"); return -1; }

    /* mic: 32 KB (Opus encode), decode: 24 KB (SILK decode), play: 4 KB */
    if (xTaskCreate(mic_task, "mic", 32768, NULL, 10, &s_mic_task) != pdTRUE ||
        xTaskCreate(opus_decode_task, "opus_dec", 24576, NULL, 8, &s_decode_task) != pdTRUE ||
        xTaskCreate(play_task, "play", 4096, NULL, 9, &s_play_task) != pdTRUE) {
        ESP_LOGE(TAG, "task create failed");
        return -1;
    }
    ESP_LOGI(TAG, "audio ready: in=%dHz out=%dHz frame=%dms",
             SAMPLE_RATE_IN, s_play_sample_rate, FRAME_MS);
    return 0;
}