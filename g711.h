#pragma once

#include <cstdint>

// Minimal G.711 encoders (PCM16 linear -> 8-bit A-law / µ-law)
// Kept dependency-free and small so we can use the same code in the module and standalone tests.

static inline uint8_t mod_audio_stream_linear_to_ulaw(int16_t pcm_val) {
    const int16_t BIAS = 0x84;
    const int16_t CLIP = 32635;

    int16_t mask;
    int16_t seg;
    uint8_t uval;

    pcm_val = (pcm_val > CLIP) ? CLIP : pcm_val;
    pcm_val = (pcm_val < -CLIP) ? -CLIP : pcm_val;

    if (pcm_val < 0) {
        pcm_val = (int16_t)(BIAS - pcm_val);
        mask = 0x7F;
    } else {
        pcm_val = (int16_t)(BIAS + pcm_val);
        mask = 0xFF;
    }

    seg = 0;
    int16_t tmp = (int16_t)(pcm_val >> 7);
    while (tmp) {
        seg++;
        tmp >>= 1;
    }
    if (seg > 7) seg = 7;

    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F));
    return (uint8_t)(uval ^ mask);
}

static inline uint8_t mod_audio_stream_linear_to_alaw(int16_t pcm_val) {
    const int16_t ALAW_CLIP = 32635;
    int16_t mask;
    int16_t seg;
    uint8_t aval;

    pcm_val = (pcm_val > ALAW_CLIP) ? ALAW_CLIP : pcm_val;
    pcm_val = (pcm_val < -ALAW_CLIP) ? -ALAW_CLIP : pcm_val;

    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        pcm_val = (int16_t)(-pcm_val - 1);
    }

    seg = 0;
    int16_t tmp = pcm_val;
    while (tmp > 0x1F) {
        seg++;
        tmp >>= 1;
    }
    if (seg > 7) seg = 7;

    if (seg < 2) {
        aval = (uint8_t)((pcm_val >> 4) & 0x0F);
    } else {
        aval = (uint8_t)(((pcm_val >> (seg + 3)) & 0x0F));
    }

    aval |= (uint8_t)(seg << 4);
    return (uint8_t)(aval ^ mask);
}
