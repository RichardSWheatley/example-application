#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>
#include "shared/shared.h"
#include "threads/blink_threads.h"
#include "threads/uart_out.h"
#include "threads/gpio_thread.h"
#include "threads/lvgl_demo.h"

/* Keep worker threads larger during debug to catch/avoid stack starvation. */
#define STACKSIZE 4096
#define LVGL_STACKSIZE 8192

/* scheduling priority used by each thread */
#define PRIORITY 7
#define LVGL_PRIORITY 9

/* Keep uart_out active for diagnostics; disable other non-graphics threads. */
K_THREAD_DEFINE(blink0_id, STACKSIZE, blink0_thread, NULL, NULL, NULL,
	PRIORITY, 0, 0);
K_THREAD_DEFINE(blink1_id, STACKSIZE, blink1_thread, NULL, NULL, NULL,
	PRIORITY, 0, 0);
K_THREAD_DEFINE(uart_out_id, STACKSIZE, uart_out_thread, NULL, NULL, NULL,
	PRIORITY, 0, 0);
K_THREAD_DEFINE(gpio_id, STACKSIZE, gpio_thread, NULL, NULL, NULL,
	PRIORITY, 0, 0);
K_THREAD_DEFINE(lvgl_demo_id, LVGL_STACKSIZE, lvgl_demo_thread, NULL, NULL, NULL,
    LVGL_PRIORITY, 0, 0);

int main(void)
{
    const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
    int wdt_channel_id = -1;
    uint32_t cause = 0U;
    int rc = hwinfo_get_reset_cause(&cause);

    if (rc == 0) {
        printk("[boot] reset cause: 0x%08x\n", (unsigned int)cause);
        (void)hwinfo_clear_reset_cause();
    } else {
        printk("[boot] reset cause unavailable (err %d)\n", rc);
    }

    if (device_is_ready(wdt)) {
        struct wdt_timeout_cfg wdt_config = {
            .window = {
                .min = 0U,
                .max = 5000U,
            },
            .callback = NULL,
            .flags = WDT_FLAG_RESET_SOC,
        };

        wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
        if (wdt_channel_id >= 0) {
            rc = wdt_setup(wdt, 0U);
            if (rc == 0) {
                printk("[boot] watchdog enabled: timeout=5000ms, feed=4750ms\n");
            } else {
                printk("[boot] watchdog setup failed (err %d)\n", rc);
                wdt_channel_id = -1;
            }
        } else {
            printk("[boot] watchdog install failed (err %d)\n", wdt_channel_id);
        }
    } else {
        printk("[boot] watchdog device not ready\n");
    }

    while (1) {
        k_sleep(K_MSEC(4750));
        if (wdt_channel_id >= 0) {
            rc = wdt_feed(wdt, wdt_channel_id);
            if (rc != 0) {
                printk("[boot] watchdog feed failed (err %d)\n", rc);
            }
        }
        printk("Main thread loop\n");
    }

    return 0;
}
