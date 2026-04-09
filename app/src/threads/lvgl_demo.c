#include "../shared/shared.h"
#include <lv_demos.h>
#include <lvgl.h>
#include <lvgl_mem.h>
#include <lvgl_zephyr.h>
#include <stdint.h>
#include <stdio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>

#include <zephyr/cache.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lvgl_demo, CONFIG_APP_LOG_LEVEL);

#define GRAPHICS_WARMUP_MS 3000U
#define GRAPHICS_RELEASED_THREADS 5U

static void lv_draw_buf_flush_cb(const lv_draw_buf_t *draw_buf,
                                 const lv_area_t *area) {
  if (!draw_buf)
    return;
  if (draw_buf->unaligned_data && draw_buf->data_size) {
    sys_cache_data_flush_range(draw_buf->unaligned_data, draw_buf->data_size);
    __DSB();
  }
}

static void lv_draw_buf_invalidate_cb(const lv_draw_buf_t *draw_buf,
                                      const lv_area_t *area) {
  if (!draw_buf)
    return;
  if (draw_buf->unaligned_data && draw_buf->data_size) {
    sys_cache_data_invd_range(draw_buf->unaligned_data, draw_buf->data_size);
    __DSB();
  }
}

void lvgl_demo_thread(void) {
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

  /* Install draw buffer cache maintenance callbacks so LVGL will flush/
   * invalidate CPU caches when buffers are used with the GPU. This helps
   * avoid corruption when LVGL allocates buffers in cached memory.
   */
  {
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_handlers();
    if (handlers) {
      handlers->flush_cache_cb = lv_draw_buf_flush_cb;
      handlers->invalidate_cache_cb = lv_draw_buf_invalidate_cb;
    }
  }

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
  next_scene_switch = sys_timepoint_calc(
      K_SECONDS(CONFIG_LV_Z_DEMO_RENDER_DYNAMIC_SCENE_TIMEOUT));
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
    static uint32_t last_loop_ms;
    uint32_t loop_start_ms = k_uptime_get_32();
    uint32_t handler_start_ms;
    uint32_t handler_dur_ms;

    handler_start_ms = k_uptime_get_32();
    lvgl_lock();
    sleep_ms = lv_timer_handler();
    lvgl_unlock();
    handler_dur_ms = k_uptime_get_32() - handler_start_ms;
    last_loop_ms = loop_start_ms;

    /*
     * Avoid a tight zero-delay loop if LVGL reports immediate work.
     * This keeps other threads responsive while debugging freezes.
     */
    if (sleep_ms == 0U) {
      sleep_ms = 1U;
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
