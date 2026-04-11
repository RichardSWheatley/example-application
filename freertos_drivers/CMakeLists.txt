# CMake module for the FreeRTOS wrappers around the Ambiq Apollo510 HAL.
#
# This target is intentionally self-contained. Consumers are expected to
# provide:
#   - FreeRTOS-Kernel (with FreeRTOS.h, queue.h, semphr.h, stream_buffer.h)
#   - AmbiqSuite Apollo510 HAL headers / sources (am_mcu_apollo.h + the
#     am_hal_mspi / am_hal_uart TUs)
#   - A port-specific IRQ vector table that forwards MSPIx_IRQn / UARTx_IRQn
#     into freertos_mspi_irq_handler() / freertos_uart_irq_handler().
#
# Usage from a parent CMakeLists.txt:
#
#   add_subdirectory(freertos_drivers)
#   target_link_libraries(my_app PRIVATE freertos_apollo510_drivers)
#   target_link_libraries(freertos_apollo510_drivers
#       PUBLIC freertos_kernel ambiq_hal_apollo510)
#
add_library(freertos_apollo510_drivers STATIC
    src/freertos_mspi.c
    src/freertos_uart.c
)

target_include_directories(freertos_apollo510_drivers
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The wrappers are strict C11.
set_target_properties(freertos_apollo510_drivers PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
)

# Turn on reasonable warnings - don't make them fatal for downstream users.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|ARMClang")
    target_compile_options(freertos_apollo510_drivers PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wundef
    )
endif()
