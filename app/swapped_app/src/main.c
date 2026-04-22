/*
 * Copyright (c) 2026 Ambiq
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(swapped_app, LOG_LEVEL_INF);

#define STACKSIZE 1024
#define PRIORITY  7

/* Blink LED at a distinctly faster rate than the primary app so you can
 * visually confirm you are running the swapped image.
 */
#define BLINK_PERIOD_MS 250

#define LED0_NODE DT_ALIAS(led0)
#define SW0_NODE  DT_ALIAS(sw0)
#define SW1_NODE  DT_ALIAS(sw1)

#if !DT_NODE_HAS_STATUS_OKAY(LED0_NODE)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif
#if !DT_NODE_HAS_STATUS_OKAY(SW0_NODE)
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif
#if !DT_NODE_HAS_STATUS_OKAY(SW1_NODE)
#error "Unsupported board: sw1 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec swap_button = GPIO_DT_SPEC_GET(SW1_NODE, gpios);

static struct gpio_callback button_cb_data;
static struct gpio_callback swap_button_cb_data;

static K_SEM_DEFINE(button_sem, 0, K_SEM_MAX_LIMIT);

/* -------------------------------------------------------------------------
 * Blink thread — blinks led0 so it's visible the swapped image is running
 * -------------------------------------------------------------------------
 */
static void blink_thread(void)
{
	int cnt = 0;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device %s not ready", led.port->name);
		return;
	}

	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("Failed to configure LED pin %d", led.pin);
		return;
	}

	LOG_INF("Swapped-app blink thread running on led0 (%d ms period)",
		BLINK_PERIOD_MS);

	while (1) {
		gpio_pin_set_dt(&led, cnt % 2);
		k_msleep(BLINK_PERIOD_MS);
		cnt++;
	}
}

/* -------------------------------------------------------------------------
 * Button thread — prints a message each time sw0 is pressed
 * -------------------------------------------------------------------------
 */
static void button_pressed_isr(const struct device *dev,
			       struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_sem_give(&button_sem);
}

static void button_thread(void)
{
	int press_count = 0;

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button device %s not ready", button.port->name);
		return;
	}

	if (gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
		LOG_ERR("Failed to configure button pin %d", button.pin);
		return;
	}

	if (gpio_pin_interrupt_configure_dt(&button,
					    GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		LOG_ERR("Failed to configure button interrupt");
		return;
	}

	gpio_init_callback(&button_cb_data, button_pressed_isr,
			   BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	LOG_INF("Swapped-app button (sw0) ready — press to log events");

	while (1) {
		k_sem_take(&button_sem, K_FOREVER);
		press_count++;
		LOG_INF("Swapped-app button pressed (count=%d)", press_count);
	}
}

/* -------------------------------------------------------------------------
 * sw1 triggers an MCUboot swap back to the primary image.
 * Requires 3 presses within 5 seconds to guard against accidental swaps.
 * -------------------------------------------------------------------------
 */
#define SWAP_REQUIRED_PRESSES 3
#define SWAP_WINDOW_MS        5000

static void swap_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("MCUboot swap requested via sw1 x%d in %dms — returning to primary",
		SWAP_REQUIRED_PRESSES, SWAP_WINDOW_MS);
	int err = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);

	if (err) {
		LOG_ERR("Failed to request MCUboot upgrade: %d", err);
		return;
	}
	LOG_INF("Swap scheduled, rebooting...");
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DEFINE(swap_work, swap_work_handler);

static void swap_button_pressed_isr(const struct device *dev,
				    struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Ring buffer of the last SWAP_REQUIRED_PRESSES press timestamps.
	 * If the oldest entry is within SWAP_WINDOW_MS of now, trigger the swap.
	 */
	static int64_t press_times[SWAP_REQUIRED_PRESSES];
	static uint8_t press_idx;
	static uint8_t press_count;

	int64_t now = k_uptime_get();

	press_times[press_idx] = now;
	press_idx = (press_idx + 1) % SWAP_REQUIRED_PRESSES;
	if (press_count < SWAP_REQUIRED_PRESSES) {
		press_count++;
	}

	if (press_count < SWAP_REQUIRED_PRESSES) {
		return;
	}

	int64_t oldest = press_times[press_idx];

	if ((now - oldest) <= SWAP_WINDOW_MS) {
		press_count = 0;
		/* Flash ops are not safe in ISR context — defer to workqueue */
		k_work_submit(&swap_work);
	}
}

static int swap_button_init(void)
{
	if (!gpio_is_ready_dt(&swap_button)) {
		LOG_ERR("Swap button device %s not ready", swap_button.port->name);
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&swap_button, GPIO_INPUT) != 0) {
		LOG_ERR("Failed to configure swap button pin %d", swap_button.pin);
		return -EIO;
	}

	if (gpio_pin_interrupt_configure_dt(&swap_button,
					    GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		LOG_ERR("Failed to configure swap button interrupt");
		return -EIO;
	}

	gpio_init_callback(&swap_button_cb_data, swap_button_pressed_isr,
			   BIT(swap_button.pin));
	gpio_add_callback(swap_button.port, &swap_button_cb_data);

	LOG_INF("Swap button (sw1) ready — press to swap back to primary image");
	return 0;
}

/* -------------------------------------------------------------------------
 * Thread definitions
 * -------------------------------------------------------------------------
 */
K_THREAD_DEFINE(blink_tid, STACKSIZE, blink_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(button_tid, STACKSIZE, button_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);

/* -------------------------------------------------------------------------
 * Main entry point for the swapped (secondary) application
 * -------------------------------------------------------------------------
 */
int main(void)
{
	printk("Swapped application booted on %s\n", CONFIG_BOARD);

	/* Confirm the image so MCUboot does not revert on next reboot */
	if (!boot_is_img_confirmed()) {
		int rc = boot_write_img_confirmed();

		if (rc == 0) {
			LOG_INF("Swapped image confirmed");
		} else {
			LOG_ERR("Failed to confirm swapped image: %d", rc);
		}
	}

	(void)swap_button_init();

	return 0;
}
