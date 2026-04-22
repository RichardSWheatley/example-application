/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include <app/lib/audio.h>
#include <app/drivers/audio_processor.h>

#ifdef CONFIG_AUDIO_PROCESSOR

/* Simple single-instance reference implementation used by tests/examples. */

struct audio_processor_cfg {
    uint32_t sample_rate;
    uint8_t bytes_per_sample;
    size_t buf_size;
};

struct audio_processor_data {
    uint8_t *buffer;
    size_t buf_len;
    bool capturing;
    int last_peak;
    double last_rms;
};

/* Static backing buffer for the single instance */
static uint8_t audio_proc_0_buffer[1024];

static int audio_processor_dev_init(const struct device *dev)
{
    struct audio_processor_data *data = dev->data;
    const struct audio_processor_cfg *cfg = dev->config;

    ARG_UNUSED(cfg);

    data->buffer = audio_proc_0_buffer;
    data->buf_len = sizeof(audio_proc_0_buffer);
    data->capturing = false;
    data->last_peak = 0;
    data->last_rms = 0.0;

    return 0;
}

static int audio_processor_start_capture_impl(const struct device *dev)
{
    struct audio_processor_data *data = dev->data;
    const struct audio_processor_cfg *cfg = dev->config;

    if (!data || !cfg) return -ENODEV;

    /* Fill the buffer with a simple synthetic waveform for example/tests */
    if (cfg->bytes_per_sample == 2) {
        int16_t *s = (int16_t *)data->buffer;
        size_t count = data->buf_len / 2;
        for (size_t i = 0; i < count; i++) {
            s[i] = (int16_t)((i % 32) * 200 - 3200); /* simple ramp pattern */
        }
    } else {
        /* for other widths, zero the buffer */
        memset(data->buffer, 0, data->buf_len);
    }

    data->last_peak = audio_peak(data->buffer, data->buf_len, cfg->bytes_per_sample);
    data->last_rms = audio_rms(data->buffer, data->buf_len, cfg->bytes_per_sample);
    data->capturing = true;

    return 0;
}

static int audio_processor_stop_capture_impl(const struct device *dev)
{
    struct audio_processor_data *data = dev->data;
    if (!data) return -ENODEV;
    data->capturing = false;
    return 0;
}

static int audio_processor_read_samples_impl(const struct device *dev, void *buf, size_t *len)
{
    struct audio_processor_data *data = dev->data;
    if (!data || !buf || !len) return -EINVAL;

    size_t to_copy = MIN(*len, data->buf_len);
    memcpy(buf, data->buffer, to_copy);
    *len = to_copy;

    return 0;
}

static int audio_processor_get_peak_impl(const struct device *dev)
{
    struct audio_processor_data *data = dev->data;
    if (!data) return 0;
    return data->last_peak;
}

static double audio_processor_get_rms_impl(const struct device *dev)
{
    struct audio_processor_data *data = dev->data;
    if (!data) return 0.0;
    return data->last_rms;
}

static bool audio_processor_get_vad_impl(const struct device *dev, int threshold)
{
    struct audio_processor_data *data = dev->data;
    const struct audio_processor_cfg *cfg = dev->config;
    if (!data || !cfg) return false;
    return audio_vad(data->buffer, data->buf_len, cfg->bytes_per_sample, threshold);
}

static const struct audio_processor_api audio_processor_api = {
    .start_capture = audio_processor_start_capture_impl,
    .stop_capture = audio_processor_stop_capture_impl,
    .read_samples = audio_processor_read_samples_impl,
    .get_peak = audio_processor_get_peak_impl,
    .get_rms = audio_processor_get_rms_impl,
    .get_vad = audio_processor_get_vad_impl,
};

static struct audio_processor_data audio_processor_data_0;
static const struct audio_processor_cfg audio_processor_cfg_0 = {
    .sample_rate = 16000,
    .bytes_per_sample = 2,
    .buf_size = sizeof(audio_proc_0_buffer),
};

DEVICE_DEFINE(audio_processor_0,
              "AUDIO_PROC_0",
              audio_processor_dev_init,
              NULL,
              &audio_processor_data_0,
              &audio_processor_cfg_0,
              POST_KERNEL,
              CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
              &audio_processor_api);

#endif /* CONFIG_AUDIO_PROCESSOR */
