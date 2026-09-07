/**
 * opus_codec.c — libopus (esp-opus component) encoder/decoder setup.
 *
 * Uplink: 16 kHz mono, CONFIG_OPUS_FRAME_DURATION_MS frames, variable bitrate.
 * Downlink: decoder is (re)initialized to whatever sample rate the server
 * announced in its hello (16 kHz or 24 kHz per the docs).
 */
#include "opus_codec.h"
#include "app.h"

#include <string.h>
#include "esp_log.h"
#include "opus.h"

static const char *TAG = "opus";

#define IN_SAMPLE_RATE 16000
#define FRAME_MS CONFIG_OPUS_FRAME_DURATION_MS
#define IN_FRAME_SAMPLES (IN_SAMPLE_RATE * FRAME_MS / 1000)

static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static int s_dec_rate;
static uint8_t s_enc_buf[512];

const uint8_t *opus_encode_frame(const int16_t *pcm, int nsamples, size_t *out_len)
{
    if (nsamples < IN_FRAME_SAMPLES) return NULL;
    int n = opus_encode(s_enc, pcm, IN_FRAME_SAMPLES, s_enc_buf, sizeof(s_enc_buf));
    if (n < 0) { ESP_LOGE(TAG, "encode error %d", n); return NULL; }
    *out_len = n;
    return s_enc_buf;
}

int opus_decode_frame(const uint8_t *data, size_t len, int16_t *pcm, int max_samples)
{
    if (s_dec_rate <= 0) return -1;
    int n = opus_decode(s_dec, data, len, pcm, max_samples, 0);
    if (n < 0) { ESP_LOGE(TAG, "decode error %d", n); return -1; }
    return n;
}

/* Called by the session layer when the server hello announces a rate.
   Named *_setup to avoid clashing with libopus's own opus_decoder_init(). */
void opus_decoder_setup(int sample_rate)
{
    if (s_dec && s_dec_rate == sample_rate) return;
    int err = 0;
    if (s_dec) { opus_decoder_destroy(s_dec); s_dec = NULL; }
    s_dec = opus_decoder_create(sample_rate, 1, &err);
    if (!s_dec || err != OPUS_OK) {
        ESP_LOGE(TAG, "decoder init failed (rate %d, err %d)", sample_rate, err);
        s_dec_rate = 0;
        return;
    }
    s_dec_rate = sample_rate;
    ESP_LOGI(TAG, "decoder @ %d Hz", sample_rate);
}

static void encoder_init(void)
{
    int err = 0;
    s_enc = opus_encoder_create(IN_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!s_enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "encoder init failed (err %d)", err);
        return;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(CONFIG_OPUS_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(1));
    ESP_LOGI(TAG, "encoder @ %d Hz, %d bps, %d ms frames",
             IN_SAMPLE_RATE, CONFIG_OPUS_BITRATE, FRAME_MS);
}

void opus_init(void)
{
    encoder_init();
}