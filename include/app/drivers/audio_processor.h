/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APP_DRIVERS_AUDIO_PROCESSOR_H_
#define APP_DRIVERS_AUDIO_PROCESSOR_H_

#include <zephyr/device.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_PROC_LABEL "AUDIO_PROC_0"

struct audio_processor_api {
    int (*start_capture)(const struct device *dev);
    int (*stop_capture)(const struct device *dev);
    int (*read_samples)(const struct device *dev, void *buf, size_t *len);
    int (*get_peak)(const struct device *dev);
    double (*get_rms)(const struct device *dev);
    bool (*get_vad)(const struct device *dev, int threshold);
};

static inline const struct audio_processor_api *audio_processor_get_api(const struct device *dev)
{
    return (const struct audio_processor_api *)dev->api;
}

static inline int audio_processor_start_capture(const struct device *dev)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->start_capture ? api->start_capture(dev) : -ENOTSUP;
}

static inline int audio_processor_stop_capture(const struct device *dev)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->stop_capture ? api->stop_capture(dev) : -ENOTSUP;
}

static inline int audio_processor_read_samples(const struct device *dev, void *buf, size_t *len)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->read_samples ? api->read_samples(dev, buf, len) : -ENOTSUP;
}

static inline int audio_processor_get_peak(const struct device *dev)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->get_peak ? api->get_peak(dev) : 0;
}

static inline double audio_processor_get_rms(const struct device *dev)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->get_rms ? api->get_rms(dev) : 0.0;
}

static inline bool audio_processor_get_vad(const struct device *dev, int threshold)
{
    const struct audio_processor_api *api = audio_processor_get_api(dev);
    return api && api->get_vad ? api->get_vad(dev, threshold) : false;
}

#endif /* APP_DRIVERS_AUDIO_PROCESSOR_H_ */
