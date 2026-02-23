#ifndef SHARED_H
#define SHARED_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

#define SLEEP_TIME_MS 1

struct printk_data_t {
    uint32_t gpio;
    uint32_t count;
};

extern struct k_msgq printk_msgq;
extern struct k_sem button_sem;
extern struct k_sem graphics_ready_sem;
extern atomic_t printk_drop_count;
extern atomic_t button_irq_count;
extern atomic_t button_handled_count;

struct led {
    struct gpio_dt_spec spec;
    uint8_t num;
};

extern struct gpio_dt_spec button;
extern struct gpio_dt_spec led;
extern struct gpio_callback button_cb_data;
extern const struct led led0;
extern const struct led led1;
extern const struct led led2;

#endif /* SHARED_H */
