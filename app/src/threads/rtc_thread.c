/*
 * RTC alarm thread.
 *
 * Based on the Zephyr RTC sample (samples/drivers/rtc) and RTC alarm
 * test (tests/drivers/rtc/rtc_api).  Sets a realistic boot time on the
 * RTC and configures a recurring 5-second alarm.  The alarm callback
 * fires from ISR context and wakes this thread via a semaphore.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include "../shared/shared.h"

LOG_MODULE_REGISTER(rtc_mgr, LOG_LEVEL_INF);

#define RTC_ALARM_INTERVAL_SEC 5U
#define RTC_ALARM_ID           0

static void rtc_thread_alarm_cb(const struct device *dev, uint16_t id,
			   void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);

	/* Wake the thread from ISR context using a semaphore. */
	k_sem_give(&rtc_alarm_sem);
}

static int set_next_alarm(const struct device *rtc)
{
	struct rtc_time now;
	int ret;

	ret = rtc_get_time(rtc, &now);
	if (ret < 0) {
		return ret;
	}

	/* Compute next absolute alarm time based on current time (h/m/s). */
	struct rtc_time alarm_time = now;
	int add = RTC_ALARM_INTERVAL_SEC;
	int s = alarm_time.tm_sec + add;
	int carry = s / 60;
	alarm_time.tm_sec = s % 60;

	int m = alarm_time.tm_min + carry;
	carry = m / 60;
	alarm_time.tm_min = m % 60;

	alarm_time.tm_hour = (alarm_time.tm_hour + carry) % 24;

	/* Use H/M/S mask to match the test behavior. */
	uint16_t mask = RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE |
			RTC_ALARM_TIME_MASK_HOUR;

	ret = rtc_alarm_set_time(rtc, RTC_ALARM_ID, mask, &alarm_time);
	return ret;
}

void rtc_thread(void)
{
	const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
	int ret;

	/* Keep non-graphics threads blocked until LVGL startup warmup completes. */
	k_sem_take(&graphics_ready_sem, K_FOREVER);

	if (!device_is_ready(rtc)) {
		LOG_ERR("RTC device not ready, aborting rtc thread");
		return;
	}

	/* Set a realistic boot time: 2026-04-08 12:00:00 (Wednesday) */
	struct rtc_time boot_time = {
		.tm_sec = 0,
		.tm_min = 0,
		.tm_hour = 12,
		.tm_mday = 8,
		.tm_mon = 4 - 1,     /* months since January (0-11) */
		.tm_year = 2026 - 1900, /* years since 1900 */
		.tm_wday = 3,         /* Wednesday */
		.tm_yday = 97,        /* April 8 = day 98, zero-indexed = 97 */
		.tm_isdst = -1,
		.tm_nsec = 0,
	};

	/* Follow rtc_api test sequence: disable callbacks, set absolute alarm,
	 * set RTC time, clear pending, then enable callback. Keep minimal
	 * logging and do not print diagnostic info.
	 */
	ret = rtc_alarm_set_callback(rtc, RTC_ALARM_ID, NULL, NULL);
	if (ret < 0 && ret != -ENOTSUP) {
		LOG_ERR("Failed to disable alarm callback (err %d)", ret);
		return;
	}

	/* Compute absolute alarm time derived from boot_time (h/m/s only) */
	struct rtc_time alarm_time = boot_time;
	int s = alarm_time.tm_sec + RTC_ALARM_INTERVAL_SEC;
	int carry = s / 60;
	alarm_time.tm_sec = s % 60;
	int m = alarm_time.tm_min + carry;
	carry = m / 60;
	alarm_time.tm_min = m % 60;
	alarm_time.tm_hour = (alarm_time.tm_hour + carry) % 24;

	uint16_t alarm_mask = RTC_ALARM_TIME_MASK_SECOND |
						  RTC_ALARM_TIME_MASK_MINUTE |
						  RTC_ALARM_TIME_MASK_HOUR;

	ret = rtc_alarm_set_time(rtc, RTC_ALARM_ID, alarm_mask, &alarm_time);
	if (ret < 0) {
		LOG_ERR("RTC alarm set failed (err %d)", ret);
		return;
	}

	/* Set RTC time */
	ret = rtc_set_time(rtc, &boot_time);
	if (ret < 0) {
		LOG_ERR("RTC set time failed (err %d)", ret);
		return;
	}

	LOG_INF("RTC time set: %04d-%02d-%02d %02d:%02d:%02d",
		boot_time.tm_year + 1900, boot_time.tm_mon + 1,
		boot_time.tm_mday, boot_time.tm_hour,
		boot_time.tm_min, boot_time.tm_sec);

	/* Clear pending state if supported; ignore errors */
	(void)rtc_alarm_is_pending(rtc, RTC_ALARM_ID);

	/* Enable callback now */
	ret = rtc_alarm_set_callback(rtc, RTC_ALARM_ID,
					 rtc_thread_alarm_cb, NULL);
	if (ret < 0) {
		LOG_ERR("RTC alarm callback setup failed (err %d)", ret);
		return;
	}

	LOG_INF("RTC alarm configured: %u second interval", RTC_ALARM_INTERVAL_SEC);

	while (1) {
		/* Wait for the alarm semaphore with a timeout slightly longer than
		 * the configured alarm interval to detect missing callbacks.
		 */
		int sem_rc = k_sem_take(&rtc_alarm_sem, K_SECONDS(RTC_ALARM_INTERVAL_SEC + 2));

		if (sem_rc == 0) {
			/* Normal path: alarm ISR gave the semaphore. */
			struct rtc_time now;
			ret = rtc_get_time(rtc, &now);
			if (ret == 0) {
				LOG_INF("RTC alarm fired: %04d-%02d-%02d %02d:%02d:%02d",
					now.tm_year + 1900, now.tm_mon + 1,
					now.tm_mday, now.tm_hour,
					now.tm_min, now.tm_sec);
			} else {
				LOG_WRN("RTC alarm fired, but get_time failed (err %d)", ret);
			}

			/* Re-arm alarm for next interval */
			ret = set_next_alarm(rtc);
			if (ret < 0) {
				LOG_ERR("RTC alarm re-arm failed (err %d)", ret);
				return;
			}
		} else {
			/* Timeout or error: report diagnostics to help root-cause the
			 * missing callback. Query supported fields, the configured alarm
			 * time, and pending status.
			 */
			LOG_WRN("RTC alarm semaphore timed out after %u seconds",
				RTC_ALARM_INTERVAL_SEC + 2);

			uint16_t supported = 0;
			ret = rtc_alarm_get_supported_fields(rtc, RTC_ALARM_ID, &supported);
			if (ret == 0) {
				LOG_INF("RTC alarm supported fields mask=0x%04x", supported);
			} else {
				LOG_WRN("rtc_alarm_get_supported_fields failed (err %d)", ret);
			}

			uint16_t mask = 0;
			struct rtc_time atime = {0};
			ret = rtc_alarm_get_time(rtc, RTC_ALARM_ID, &mask, &atime);
			if (ret == 0) {
				LOG_INF("RTC alarm config mask=0x%04x time=%02d:%02d:%02d",
					mask, atime.tm_hour, atime.tm_min, atime.tm_sec);
			} else {
				LOG_WRN("rtc_alarm_get_time failed (err %d)", ret);
			}

			int pending = rtc_alarm_is_pending(rtc, RTC_ALARM_ID);
			if (pending < 0) {
				LOG_WRN("rtc_alarm_is_pending failed (err %d)", pending);
			} else if (pending) {
				LOG_INF("RTC alarm is pending (was triggered but callback disabled)");
			} else {
				LOG_INF("RTC alarm is not pending");
			}

			/* Try re-arming the alarm to continue operation/tests. */
			ret = set_next_alarm(rtc);
			if (ret < 0) {
				LOG_ERR("RTC alarm re-arm failed (err %d)", ret);
				return;
			}
		}
	}
}
