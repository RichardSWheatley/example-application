#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <inttypes.h>
#include "../shared/shared.h"

void button_pressed(const struct device *dev, struct gpio_callback *cb,
            uint32_t pins)
{
    printk("Button pressed at %" PRIu32 "\n", k_cycle_get_32());
}

void gpio_thread(void)
{
    int status;

    if (!gpio_is_ready_dt(&button)) {
        printk("Error: button device %s is not ready\n",
               button.port->name);
    }

    status = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (status != 0) {
        printk("Error %d: failed to configure %s pin %d\n",
               status, button.port->name, button.pin);
    }

    status = gpio_pin_interrupt_configure_dt(&button,
                          GPIO_INT_EDGE_TO_ACTIVE);
    if (status != 0) {
        printk("Error %d: failed to configure interrupt on %s pin %d\n",
            status, button.port->name, button.pin);
    }

    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    printk("Set up button at %s pin %d\n", button.port->name, button.pin);

    if (led.port && !gpio_is_ready_dt(&led)) {
        printk("Error %d: LED device %s is not ready; ignoring it\n",
               status, led.port->name);
        led.port = NULL;
    }
    if (led.port) {
        status = gpio_pin_configure_dt(&led, GPIO_OUTPUT);
        if (status != 0) {
            printk("Error %d: failed to configure LED device %s pin %d\n",
                   status, led.port->name, led.pin);
            led.port = NULL;
        } else {
            printk("Set up LED at %s pin %d\n", led.port->name, led.pin);
        }
    }

    printk("Press the button\n");
    if (led.port) {
        while (1) {
            int val = gpio_pin_get_dt(&button);

            if (val >= 0) {
                gpio_pin_set_dt(&led, val);
            }
            k_msleep(SLEEP_TIME_MS);
        }
    }
}
