/**
 * opus_codec.h — thin wrapper over libopus (esp-opus component) for the
 * 16 kHz mono uplink / downlink streams used by the protocol.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a pointer to the encoded Opus frame (owned by this module, valid
 * until the next call) or NULL on error. */
const uint8_t *opus_encode_frame(const int16_t *pcm, int nsamples, size_t *out_len);

/* Decodes one Opus frame to 16-bit mono PCM at the given target sample rate.
 * Returns number of samples written, or -1. */
int opus_decode_frame(const uint8_t *data, size_t len, int16_t *pcm, int max_samples);

/* Resets the decoder's internal state (clears CELT/SILK memories).  Call
 * at the start of each TTS segment to avoid cross-session artefacts. */
void opus_decoder_reset(void);

#ifdef __cplusplus
}
#endif