#ifndef SHARED_H
#define SHARED_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

#define SLEEP_TIME_MS 1

/* legacy numeric printk messages removed; use Zephyr logging */

struct dbg_uart_event_t {
    const char *hypothesis_id;
    const char *location;
    const char *message;
    int32_t v1;
    int32_t v2;
    int32_t v3;
    uint32_t timestamp_ms;
};

extern struct k_sem button_sem;
extern struct k_sem graphics_ready_sem;
extern atomic_t printk_drop_count;
extern atomic_t button_irq_count;
extern atomic_t button_handled_count;

void dbg_uart_emit(const char *hypothesis_id, const char *location, const char *message,
                   int32_t v1, int32_t v2, int32_t v3);

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
