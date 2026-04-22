/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <app/drivers/audio_processor.h>

int main(void)
{
    const struct device *dev = device_get_binding(AUDIO_PROC_LABEL);
    if (!dev) {
        printk("audio_processor device not found\n");
        return -1;
    }

    printk("Starting audio capture\n");
    if (audio_processor_start_capture(dev) != 0) {
        printk("start_capture failed\n");
        return -1;
    }

    /* Allow some time to collect samples */
    k_sleep(K_MSEC(200));

    int peak = audio_processor_get_peak(dev);
    double rms = audio_processor_get_rms(dev);
    bool vad = audio_processor_get_vad(dev, 100);

    printk("Audio stats: peak=%d, rms=%f, vad=%d\n", peak, rms, vad ? 1 : 0);

    audio_processor_stop_capture(dev);
    printk("Stopped audio capture\n");
    
    return 0;
}
