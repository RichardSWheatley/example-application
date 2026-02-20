#ifndef SHARED_H
#define SHARED_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#define SLEEP_TIME_MS 1

struct printk_data_t {
    void *fifo_reserved; /* 1st word reserved for use by fifo */
    uint32_t gpio;
    uint32_t count;
};

extern struct k_fifo printk_fifo;

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
