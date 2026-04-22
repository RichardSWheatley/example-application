/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "../../include/app/lib/audio.h"

int audio_peak(const void *buf, size_t len, int bytes_per_sample)
{
    if (!buf || bytes_per_sample <= 0)
        return 0;

    const uint8_t *b = buf;
    size_t count = len / bytes_per_sample;
    int max_abs = 0;

    for (size_t i = 0; i < count; i++) {
        int32_t v = 0;
        size_t off = i * bytes_per_sample;
        switch (bytes_per_sample) {
        case 1:
            v = (int8_t)b[off];
            break;
        case 2:
            v = (int16_t)(b[off] | (b[off+1] << 8));
            break;
        case 3:
            v = (int32_t)(b[off] | (b[off+1] << 8) | (b[off+2] << 16));
            /* sign-extend 24-bit */
            if (v & 0x00800000)
                v |= ~0x00FFFFFF;
            break;
        case 4:
            v = (int32_t)(b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24));
            break;
        default:
            /* unsupported, treat bytes as unsigned */
            for (int j = 0; j < bytes_per_sample && off + j < len; j++)
                v = (v << 8) | b[off + j];
            break;
        }

        int abs_v = v < 0 ? -v : v;
        if (abs_v > max_abs)
            max_abs = abs_v;
    }

    return max_abs;
}

double audio_rms(const void *buf, size_t len, int bytes_per_sample)
{
    if (!buf || bytes_per_sample <= 0)
        return 0.0;

    const uint8_t *b = buf;
    size_t count = len / bytes_per_sample;
    if (count == 0)
        return 0.0;

    double acc = 0.0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = 0;
        size_t off = i * bytes_per_sample;
        switch (bytes_per_sample) {
        case 1:
            v = (int8_t)b[off];
            break;
        case 2:
            v = (int16_t)(b[off] | (b[off+1] << 8));
            break;
        case 3:
            v = (int32_t)(b[off] | (b[off+1] << 8) | (b[off+2] << 16));
            if (v & 0x00800000)
                v |= ~0x00FFFFFF;
            break;
        case 4:
            v = (int32_t)(b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24));
            break;
        default:
            for (int j = 0; j < bytes_per_sample && off + j < len; j++)
                v = (v << 8) | b[off + j];
            break;
        }

        acc += (double)v * (double)v;
    }

    acc = acc / (double)count;
    return sqrt(acc);
}

bool audio_vad(const void *buf, size_t len, int bytes_per_sample, int threshold)
{
    int peak = audio_peak(buf, len, bytes_per_sample);
    return peak >= threshold;
}
