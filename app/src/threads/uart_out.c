#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "../shared/shared.h"

void uart_out_thread(void)
{
    struct printk_data_t rx_data;

    /* Keep non-graphics output blocked until LVGL startup warmup completes. */
    k_sem_take(&graphics_ready_sem, K_FOREVER);

    while (1) {
         k_msgq_get(&printk_msgq, &rx_data, K_FOREVER);
         printk("Toggled led%d; counter=%d\n",
            rx_data.gpio, rx_data.count);

        /* Periodically report queue/event diagnostics */
        static uint32_t last_report_ms = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_report_ms > 5000) {
            uint32_t drops = (uint32_t)atomic_get(&printk_drop_count);
            uint32_t irq_count = (uint32_t)atomic_get(&button_irq_count);
            uint32_t handled_count = (uint32_t)atomic_get(&button_handled_count);
            uint32_t pending = (irq_count >= handled_count) ? (irq_count - handled_count) : 0U;
            uint32_t queued = (uint32_t)k_msgq_num_used_get(&printk_msgq);
            printk("[diag] drops=%u button_irq=%u handled=%u pending=%u queued=%u\n",
                   drops, irq_count, handled_count, pending, queued);

            last_report_ms = now;
        }
    }
}
