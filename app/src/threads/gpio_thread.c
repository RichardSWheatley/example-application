#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gpio, LOG_LEVEL_INF);
#include "../shared/shared.h"
#include <inttypes.h>

/* Require 3 presses of sw1 within 5 seconds to trigger an MCUboot swap.
 * This guards against accidental swaps from a single button press.
 */
#define SWAP_REQUIRED_PRESSES 3
#define SWAP_WINDOW_MS        5000

static void swap_work_handler(struct k_work *work)
{
  ARG_UNUSED(work);

  LOG_INF("MCUboot swap requested via button (sw1 x%d in %dms)",
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

void button_pressed(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins) {
  ARG_UNUSED(dev);
  ARG_UNUSED(cb);

  atomic_inc(&button_irq_count);

  /* Wake the gpio thread to handle the button event. */
  k_sem_give(&button_sem);
}

static void swap_button_pressed(const struct device *dev,
                                struct gpio_callback *cb, uint32_t pins) {
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

  /* Oldest timestamp is the one we are about to overwrite next */
  int64_t oldest = press_times[press_idx];

  if ((now - oldest) <= SWAP_WINDOW_MS) {
    /* Reset so a successful trigger can't fire twice if the work is slow */
    press_count = 0;
    /* Flash ops are not safe in ISR context — defer to the system workqueue */
    k_work_submit(&swap_work);
  }
}

void gpio_thread(void) {
  int status;
  bool led_available = (led.port != NULL);

  /* Keep non-graphics threads blocked until LVGL startup warmup completes. */
  k_sem_take(&graphics_ready_sem, K_FOREVER);

  if (!gpio_is_ready_dt(&button)) {
    LOG_ERR("Error: button device %s is not ready", button.port->name);
    return;
  }

  status = gpio_pin_configure_dt(&button, GPIO_INPUT);
  if (status != 0) {
    LOG_ERR("Error %d: failed to configure %s pin %d", status,
            button.port->name, button.pin);
    return;
  }

  status = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
  if (status != 0) {
    LOG_ERR("Error %d: failed to configure interrupt on %s pin %d", status,
            button.port->name, button.pin);
    return;
  }

  gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
  gpio_add_callback(button.port, &button_cb_data);
  LOG_INF("Set up button at %s pin %d", button.port->name, button.pin);

  /* Set up sw1 as the MCUboot swap trigger button */
  if (!gpio_is_ready_dt(&swap_button)) {
    LOG_ERR("Error: swap button device %s is not ready", swap_button.port->name);
  } else {
    status = gpio_pin_configure_dt(&swap_button, GPIO_INPUT);
    if (status != 0) {
      LOG_ERR("Error %d: failed to configure swap button %s pin %d", status,
              swap_button.port->name, swap_button.pin);
    } else {
      status = gpio_pin_interrupt_configure_dt(&swap_button,
                                               GPIO_INT_EDGE_TO_ACTIVE);
      if (status != 0) {
        LOG_ERR("Error %d: failed to configure interrupt on swap button", status);
      } else {
        gpio_init_callback(&swap_button_cb_data, swap_button_pressed,
                           BIT(swap_button.pin));
        gpio_add_callback(swap_button.port, &swap_button_cb_data);
        LOG_INF("Set up swap button (sw1) at %s pin %d — press to trigger MCUboot swap",
                swap_button.port->name, swap_button.pin);
      }
    }
  }

  if (led_available && !gpio_is_ready_dt(&led)) {
    LOG_ERR("Error %d: LED device %s is not ready; ignoring it", status,
            led.port->name);
    led_available = false;
  }
  if (led_available) {
    status = gpio_pin_configure_dt(&led, GPIO_OUTPUT);
    if (status != 0) {
      LOG_ERR("Error %d: failed to configure LED device %s pin %d", status,
              led.port->name, led.pin);
      led_available = false;
    } else {
      LOG_INF("Set up LED at %s pin %d", led.port->name, led.pin);
    }
  }

  LOG_INF("Press the button");
  if (led_available) {
    while (1) {
      /* wait for button press signalled by ISR */
      k_sem_take(&button_sem, K_FOREVER);
      atomic_inc(&button_handled_count);
      int val = gpio_pin_get_dt(&button);
      if (val < 0) {
        LOG_ERR("Failed to read button value (err %d)", val);
        continue;
      }

      LOG_INF("Button pressed, value=%d", val);

      /* Toggle the LED state on each press */
      int led_val = gpio_pin_get_dt(&led);
      if (led_val < 0) {
        /* If reading fails, try setting to the button value */
        if (gpio_pin_set_dt(&led, val) < 0) {
          LOG_ERR("Failed to set LED to %d", val);
        }
      } else {
        int new_led = !led_val;
        if (gpio_pin_set_dt(&led, new_led) < 0) {
          LOG_ERR("Failed to toggle LED (tried %d)", new_led);
        } else {
          LOG_INF("LED toggled to %d", new_led);
        }
      }

      k_msleep(SLEEP_TIME_MS);
    }
  }
}
