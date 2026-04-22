/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APP_LIB_AUDIO_H_
#define APP_LIB_AUDIO_H_

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Compute peak (maximum absolute) sample value from a buffer.
 * @param buf Pointer to buffer holding interleaved samples
 * @param len Buffer length in bytes
 * @param bytes_per_sample 1,2,3 or 4
 * @return maximum absolute sample value (signed interpretation)
 */
extern int audio_peak(const void *buf, size_t len, int bytes_per_sample);

/**
 * @brief Compute RMS (root-mean-square) of samples in buffer.
 */
extern double audio_rms(const void *buf, size_t len, int bytes_per_sample);

/**
 * @brief Simple voice-activity detection using peak threshold.
 */
extern bool audio_vad(const void *buf, size_t len, int bytes_per_sample, int threshold);

#endif /* APP_LIB_AUDIO_H_ */
