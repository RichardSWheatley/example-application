# Copyright (c) 2026 Ambiq Micro Inc.
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_BOOTLOADER_MCUBOOT OR CONFIG_MCUBOOT)
board_runner_args(jlink "--device=AP510NFA-CBR" "--iface=swd" "--speed=1000")
else()
board_runner_args(jlink "--device=AP510NFA-CBR" "--iface=swd" "--speed=1000" "--erase")
endif()

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../common/example_runner.board.cmake)
