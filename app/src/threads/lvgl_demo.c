#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <lvgl_mem.h>
#include <lvgl_zephyr.h>
#include <lv_demos.h>
#include <stdio.h>
#include <stdint.h>
#include "../shared/shared.h"

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lvgl_demo);

#define GRAPHICS_WARMUP_MS 5000U
#define GRAPHICS_RELEASED_THREADS 4U

void lvgl_demo_thread(void)
{
    const struct device *display_dev;
#ifdef CONFIG_LV_Z_DEMO_RENDER_SCENE_DYNAMIC
    k_timepoint_t next_scene_switch;
    lv_demo_render_scene_t cur_scene = LV_DEMO_RENDER_SCENE_FILL;
#endif

    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready, aborting LVGL demo");
        return;
    }

    LOG_INF("LVGL demo thread starting on %p", display_dev);

    lvgl_lock();

#if defined(CONFIG_LV_Z_DEMO_MUSIC)
    lv_demo_music();
#elif defined(CONFIG_LV_Z_DEMO_BENCHMARK)
    lv_demo_benchmark();
#elif defined(CONFIG_LV_Z_DEMO_STRESS)
    lv_demo_stress();
#elif defined(CONFIG_LV_Z_DEMO_WIDGETS)
    lv_demo_widgets();
#elif defined(CONFIG_LV_Z_DEMO_KEYPAD_AND_ENCODER)
    lv_demo_keypad_encoder();
#elif defined(CONFIG_LV_Z_DEMO_RENDER)

#ifdef CONFIG_LV_Z_DEMO_RENDER_SCENE_DYNAMIC
    lv_demo_render(cur_scene, 255);
    next_scene_switch = sys_timepoint_calc(K_SECONDS(CONFIG_LV_Z_DEMO_RENDER_DYNAMIC_SCENE_TIMEOUT));
#else
    lv_demo_render(CONFIG_LV_Z_DEMO_RENDER_SCENE_INDEX, 255);
#endif

#else
#error Enable one of the demos CONFIG_LV_Z_DEMO_*
#endif

#ifndef CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE
    lv_timer_handler();
#endif
    lvgl_unlock();

    display_blanking_off(display_dev);
#if defined(CONFIG_LOG)
    LOG_INF("LVGL demo entering main loop");
#endif
#ifdef CONFIG_LV_Z_MEM_POOL_SYS_HEAP
    lvgl_print_heap_info(false);
#else
    printf("lvgl in malloc mode\n");
#endif

    /*
     * Give graphics a brief warmup window before other app threads proceed.
     * This acts as a deterministic startup barrier.
     */
    k_msleep(GRAPHICS_WARMUP_MS);
    for (uint32_t i = 0; i < GRAPHICS_RELEASED_THREADS; i++) {
        k_sem_give(&graphics_ready_sem);
    }

    while (1) {
#ifndef CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE
        uint32_t sleep_ms;

        lvgl_lock();
        sleep_ms = lv_timer_handler();
        lvgl_unlock();

        /*
         * Avoid a tight zero-delay loop if LVGL reports immediate work.
         * This keeps other threads responsive while debugging freezes.
         */
        if (sleep_ms == 0U) {
            sleep_ms = 2U;
        }

        /* Heartbeat so we can tell if LVGL loop is still running */
        static uint32_t last_report = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_report > 5000U) {
            LOG_INF("LVGL heartbeat");
            last_report = now;
        }

        k_msleep(MIN(sleep_ms, INT32_MAX));
#else
        k_msleep(10);
#endif

#ifdef CONFIG_LV_Z_DEMO_RENDER_SCENE_DYNAMIC
        if (sys_timepoint_expired(next_scene_switch)) {
            cur_scene = (cur_scene + 1) % LV_DEMO_RENDER_SCENE_NUM;
            lvgl_lock();
            lv_demo_render(cur_scene, 255);
            lvgl_unlock();
            next_scene_switch = sys_timepoint_calc(
                K_SECONDS(CONFIG_LV_Z_DEMO_RENDER_DYNAMIC_SCENE_TIMEOUT));
        }
#endif
    }
}
