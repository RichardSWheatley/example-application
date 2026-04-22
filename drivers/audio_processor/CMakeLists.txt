# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_AUDIO_PROCESSOR audio_processor.c)
