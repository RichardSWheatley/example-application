#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(blink, LOG_LEVEL_INF);
#include <zephyr/sys/__assert.h>
#include "../shared/shared.h"

static void blink(const struct led *ledp, uint32_t sleep_ms, uint32_t id)
{
    const struct gpio_dt_spec *spec = &ledp->spec;
    int cnt = 0;
    int ret;

    if (!device_is_ready(spec->port)) {
        LOG_ERR("Error: %s device is not ready", spec->port->name);
        return;
    }

    ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT);
    if (ret != 0) {
        LOG_ERR("Error %d: failed to configure pin %d (LED '%d')",
               ret, spec->pin, ledp->num);
        return;
    }

    /* Keep non-graphics threads blocked until LVGL startup warmup completes. */
    k_sem_take(&graphics_ready_sem, K_FOREVER);

    while (1) {
        gpio_pin_set_dt(spec, cnt % 2);

        LOG_INF("Toggled led%u; counter=%d", (unsigned)ledp->num, cnt);

        k_msleep(sleep_ms);
        cnt++;
    }
}

void blink0_thread(void)
{
    blink(&led1, 2000, 0);
}

void blink1_thread(void)
{
    blink(&led2, 5000, 1);
}
