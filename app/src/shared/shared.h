#ifndef SHARED_H
#define SHARED_H

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define SLEEP_TIME_MS 1

extern struct k_sem button_sem;
extern struct k_sem graphics_ready_sem;
extern struct k_sem rtc_alarm_sem;
extern atomic_t button_irq_count;
extern atomic_t button_handled_count;

struct led {
  struct gpio_dt_spec spec;
  uint8_t num;
};

extern const struct gpio_dt_spec button;
extern const struct gpio_dt_spec led;
extern struct gpio_callback button_cb_data;
extern const struct led led0;
extern const struct led led1;
extern const struct led led2;

#endif /* SHARED_H */
