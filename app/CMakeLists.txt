#-------------------------------------------------------------------------------
# Zephyr Example Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# Allow standalone builds when this repo is not listed in the workspace manifest.
get_filename_component(APP_MODULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES "${APP_MODULE_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})

target_sources(app PRIVATE 
  src/main.c
  src/shared/shared.c
  src/threads/blink_threads.c
  src/threads/uart_out.c
  src/threads/gpio_thread.c
  src/threads/lvgl_demo.c
)

target_include_directories(app PRIVATE
  ${LVGL_DIR}/demos/
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_MUSIC app PRIVATE
    ${LVGL_DIR}/demos/music/lv_demo_music_list.c
    ${LVGL_DIR}/demos/music/lv_demo_music.c
    ${LVGL_DIR}/demos/music/lv_demo_music_main.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_next.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_wave_top_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_4_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_4.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_corner_right.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_1.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_slider_knob_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_3.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_pause.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_pause_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_wave_bottom_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_2_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_list_play_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_wave_top.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_play_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_1_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_wave_bottom.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_corner_left_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_play.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_list_border.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_2.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_next_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_list_play.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_list_border_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_rnd.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_3.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_prev_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_loop.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_1.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_slider_knob.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_corner_right_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_corner_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_corner_left.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_2_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_3_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_icon_1_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_2.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_prev.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_rnd_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_list_pause_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_cover_3_large.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_list_pause.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_logo.c
    ${LVGL_DIR}/demos/music/assets/img_lv_demo_music_btn_loop_large.c
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_BENCHMARK app PRIVATE
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_avatar.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_argb.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_rgb.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_12_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_14_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_16_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_18_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_20_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_24_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_26_aligned.c
    ${LVGL_DIR}/demos/benchmark/lv_demo_benchmark.c
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_STRESS app PRIVATE
    ${LVGL_DIR}/demos/stress/lv_demo_stress.c
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_WIDGETS app PRIVATE
    ${LVGL_DIR}/demos/widgets/assets/img_clothes.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_avatar.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_needle.c
    ${LVGL_DIR}/demos/widgets/assets/img_lvgl_logo.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets.c
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_KEYPAD_AND_ENCODER app PRIVATE
    ${LVGL_DIR}/demos/keypad_encoder/lv_demo_keypad_encoder.c
)

target_sources_ifdef(CONFIG_LV_USE_DEMO_RENDER app PRIVATE
    ${LVGL_DIR}/demos/render/assets/img_render_arc_bg.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_argb8888.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_argb8888_premultiplied.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_i1.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_l8.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_rgb565a8.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_rgb565.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_rgb565_swapped.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_rgb888.c
    ${LVGL_DIR}/demos/render/assets/img_render_lvgl_logo_xrgb8888.c
    ${LVGL_DIR}/demos/render/lv_demo_render.c
)

# The code below locates the git index file for this repository and adds it as a dependency for
# the application VERSION file so that if the repo has a new commit added, even if no files in
# the build have changed, the application version file will be regenerated with the new git commit
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY .
    OUTPUT_VARIABLE application_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE stderr
    RESULT_VARIABLE return_code
  )
  # If there is an error e.g. it is not a git repo, we just silently ignore it and continue
  # without a dependency, this will be the case with freestanding applications.
  if(NOT return_code)
    if(NOT "${stderr}" STREQUAL "")
      message(WARNING "Application build version git rev-parse warned: ${stderr}")
    endif()

    set_property(TARGET app_version_h PROPERTY APP_VERSION_DEPENDS ${application_git_dir}/index)
  endif()
endif()
