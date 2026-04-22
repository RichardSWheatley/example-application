# Copyright (c) 2026 Ambiq
# SPDX-License-Identifier: Apache-2.0

# Use the same MCUboot signing key for both primary and secondary images
set(swapped_app_CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
  \"${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}\" CACHE STRING
  "Signature key file for signing" FORCE)

# Add the swapped app to the build
ExternalZephyrProject_Add(
  APPLICATION swapped_app
  SOURCE_DIR ${APP_DIR}/swapped_app
)

# Pass sysbuild MCUBOOT swap-type configuration to the swapped app
set_target_properties(swapped_app PROPERTIES
  IMAGE_CONF_SCRIPT ${ZEPHYR_BASE}/share/sysbuild/image_configurations/MAIN_image_default.cmake
)

# Ensure flashing order: mcuboot -> swapped_app -> primary app
# This prevents MCUboot from triggering a swap before the secondary
# image is actually present in flash.
sysbuild_add_dependencies(FLASH app swapped_app)
sysbuild_add_dependencies(FLASH swapped_app mcuboot)
