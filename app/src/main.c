#include <zephyr/kernel.h>
#include "shared/shared.h"
#include "threads/blink_threads.h"
#include "threads/uart_out.h"
#include "threads/gpio_thread.h"
#include "threads/lvgl_demo.h"

/* size of stack area used by each thread */
/* increase from 1KB to 4KB to avoid stack overflows in demo threads */
#define STACKSIZE 1024
#define LVGL_STACKSIZE 8192

/* scheduling priority used by each thread */
#define PRIORITY 7

K_THREAD_DEFINE(blink0_id, STACKSIZE, blink0_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(blink1_id, STACKSIZE, blink1_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(uart_out_id, STACKSIZE, uart_out_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(gpio_id, STACKSIZE, gpio_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
K_THREAD_DEFINE(lvgl_demo_id, LVGL_STACKSIZE, lvgl_demo_thread, NULL, NULL, NULL,
	PRIORITY, 0, 0);