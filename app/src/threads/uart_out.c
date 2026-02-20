#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "../shared/shared.h"

void uart_out_thread(void)
{
    while (1) {
        struct printk_data_t *rx_data = k_fifo_get(&printk_fifo,
                                                   K_FOREVER);
        printk("Toggled led%d; counter=%d\n",
               rx_data->gpio, rx_data->count);
        k_free(rx_data);
    }
}
