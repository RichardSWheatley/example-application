# Copyright (c) 2026 Ambiq Micro Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MSPI_FREERTOS_APOLLO510
			     ambiq_apollo510_mspi.c)
