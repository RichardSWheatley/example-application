#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <string.h>
#include "../shared/shared.h"

static void blink(const struct led *ledp, uint32_t sleep_ms, uint32_t id)
{
    const struct gpio_dt_spec *spec = &ledp->spec;
    int cnt = 0;
    int ret;

    if (!device_is_ready(spec->port)) {
        printk("Error: %s device is not ready\n", spec->port->name);
        return;
    }

    ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT);
    if (ret != 0) {
        printk("Error %d: failed to configure pin %d (LED '%d')\n",
               ret, spec->pin, ledp->num);
        return;
    }

    while (1) {
        gpio_pin_set_dt(spec, cnt % 2);

        struct printk_data_t tx_data = { .gpio = id, .count = cnt };

        size_t size = sizeof(struct printk_data_t);
        char *mem_ptr = k_malloc(size);
        __ASSERT_NO_MSG(mem_ptr != 0);

        memcpy(mem_ptr, &tx_data, size);

        k_fifo_put(&printk_fifo, mem_ptr);

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
