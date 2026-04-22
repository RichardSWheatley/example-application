/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <app/lib/audio.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>

int main(void)
{
    int32_t samples[] = {0, -100, 456, -1000};
    int peak = audio_peak(samples, sizeof(samples), 4);
    double r = audio_rms(samples, sizeof(samples), 4);
    bool vad1 = audio_vad(samples, sizeof(samples), 4, 500);
    bool vad2 = audio_vad(samples, sizeof(samples), 4, 1500);

    printk("Audio utils: peak=%d, rms=%f, vad(500)=%d, vad(1500)=%d\n",
           peak, r, vad1 ? 1 : 0, vad2 ? 1 : 0);
    
    return 0;
}
