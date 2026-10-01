#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include "shared/shared.h"
#include "threads/blink_threads.h"
#include "threads/gpio_thread.h"
#include "threads/rtc_thread.h"
#include <zephyr/logging/log.h>

#if defined(CONFIG_BT)
#include "threads/ble_hr_thread.h"
#endif

#if defined(CONFIG_MSPI)
#include "threads/mspi_stress_thread.h"
#endif

#if defined(CONFIG_LVGL)
#include "threads/lvgl_demo.h"
#endif

#if defined(CONFIG_AUDIO_DMIC)
#include "threads/audio_thread.h"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define STACKSIZE 1024
#define BLE_STACKSIZE 2048
#define MSPI_STACKSIZE 8192
#define AUDIO_STACKSIZE 4096
#define LVGL_STACKSIZE 8192

#define PRIORITY 7
#define BLE_PRIORITY 7
#define MSPI_PRIORITY 7
#define AUDIO_PRIORITY 8
#define LVGL_PRIORITY 9

/* Must match GRAPHICS_RELEASED_THREADS in lvgl_demo.c */
#define GRAPHICS_RELEASED_THREADS 6U

K_THREAD_DEFINE(blink0_id, STACKSIZE, blink0_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(blink1_id, STACKSIZE, blink1_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(gpio_id, STACKSIZE, gpio_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(rtc_id, STACKSIZE, rtc_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);

#if defined(CONFIG_BT)
K_THREAD_DEFINE(ble_hr_id, BLE_STACKSIZE, ble_hr_thread, NULL, NULL, NULL,
		BLE_PRIORITY, 0, 0);
#endif

#if defined(CONFIG_MSPI)
K_THREAD_DEFINE(mspi_stress_id, MSPI_STACKSIZE, mspi_stress_thread, NULL, NULL,
		NULL, MSPI_PRIORITY, 0, 0);
#endif

#if defined(CONFIG_AUDIO_DMIC)
K_THREAD_DEFINE(audio_id, AUDIO_STACKSIZE, audio_thread, NULL, NULL, NULL,
		AUDIO_PRIORITY, 0, 0);
#endif

#if defined(CONFIG_LVGL)
K_THREAD_DEFINE(lvgl_demo_id, LVGL_STACKSIZE, lvgl_demo_thread, NULL, NULL, NULL,
		LVGL_PRIORITY, 0, 0);
#endif

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	int wdt_channel_id = -1;
	uint32_t cause = 0U;
	int rc = hwinfo_get_reset_cause(&cause);

	if (rc == 0) {
		LOG_INF("[boot] reset cause: 0x%08x", (unsigned int)cause);
	} else {
		LOG_ERR("[boot] reset cause unavailable (err %d)", rc);
	}

	if (device_is_ready(wdt)) {
		struct wdt_timeout_cfg wdt_config = {
			.window = {
				.min = 0U,
				.max = 10000U,
			},
			.callback = NULL,
			.flags = WDT_FLAG_RESET_SOC,
		};

		wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
		if (wdt_channel_id >= 0) {
			rc = wdt_setup(wdt, 0U);
			if (rc == 0) {
				LOG_INF("[boot] watchdog enabled: timeout=10000ms, feed=5000ms");
			} else {
				LOG_ERR("[boot] watchdog setup failed (err %d)", rc);
				wdt_channel_id = -1;
			}
		} else {
			LOG_ERR("[boot] watchdog install failed (err %d)", wdt_channel_id);
		}
	} else {
		LOG_ERR("[boot] watchdog device not ready");
	}

#if !defined(CONFIG_LVGL)
	/*
	 * Worker threads wait on graphics_ready_sem. With no LVGL/display,
	 * release them here so blink/gpio/rtc can run.
	 */
	for (uint32_t i = 0; i < GRAPHICS_RELEASED_THREADS; i++) {
		k_sem_give(&graphics_ready_sem);
	}
	LOG_INF("[boot] no display/LVGL; released worker threads");
#endif

	while (1) {
		k_sleep(K_MSEC(5000));
		if (wdt_channel_id >= 0) {
			rc = wdt_feed(wdt, wdt_channel_id);
			if (rc != 0) {
				LOG_ERR("[boot] watchdog feed failed (err %d)", rc);
			}
		}
		LOG_DBG("Main thread loop");
	}

	return 0;
}
