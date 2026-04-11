/*
 * freertos_uart.h - FreeRTOS wrapper over the Ambiq Apollo510 UART HAL
 *
 * Thread-safe, OS-aware UART abstraction built on the AmbiqSuite low-level
 * UART HAL (am_hal_uart_*). Two independent queues (TX, RX) decouple
 * application threads from ISR work; send/receive APIs block on OS primitives
 * with caller-provided timeouts instead of polling flags.
 *
 * Design summary:
 *
 *   RX path:
 *       ISR --> am_hal_uart_interrupt_service() drains bytes into a
 *       FreeRTOS StreamBuffer. Reader tasks call freertos_uart_read() and
 *       block on xStreamBufferReceive.
 *
 *   TX path:
 *       Writer task --> TX StreamBuffer --> ISR pushes bytes into the
 *       peripheral FIFO. Optional freertos_uart_flush() waits on a binary
 *       semaphore released when the stream buffer empties AND the HW FIFO
 *       drains.
 *
 *   Concurrency:
 *       Mutex around HAL configuration operations. The TX/RX stream buffers
 *       are already thread-safe across producer/consumer so no additional
 *       mutexes are needed for regular reads/writes.
 *
 *   Zero busy-wait:
 *       None of the APIs spin on hardware flags. All blocking is done on
 *       FreeRTOS primitives with a timeout.
 */

#ifndef FREERTOS_UART_H
#define FREERTOS_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"

#include "am_mcu_apollo.h"   /* pulls in am_hal_uart.h */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time defaults                                                      */
/* -------------------------------------------------------------------------- */

#ifndef FREERTOS_UART_TX_BUF_BYTES
#define FREERTOS_UART_TX_BUF_BYTES          512u
#endif

#ifndef FREERTOS_UART_RX_BUF_BYTES
#define FREERTOS_UART_RX_BUF_BYTES          512u
#endif

/* HAL internal queues (the non-blocking state machine needs scratch space). */
#ifndef FREERTOS_UART_HAL_TX_BYTES
#define FREERTOS_UART_HAL_TX_BYTES          256u
#endif
#ifndef FREERTOS_UART_HAL_RX_BYTES
#define FREERTOS_UART_HAL_RX_BYTES          256u
#endif

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

typedef enum {
    FREERTOS_UART_OK            =  0,
    FREERTOS_UART_ERR_PARAM     = -1,
    FREERTOS_UART_ERR_STATE     = -2,
    FREERTOS_UART_ERR_HAL       = -3,
    FREERTOS_UART_ERR_TIMEOUT   = -4,
    FREERTOS_UART_ERR_NOMEM     = -5,
    FREERTOS_UART_ERR_OVERRUN   = -6,
} freertos_uart_status_t;

/** Opaque instance. */
typedef struct freertos_uart_dev freertos_uart_dev_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a FreeRTOS-owned UART instance.
 *
 * @param module   UART module number (0, 1, ...).
 * @param cfg      HAL configuration (baud, parity, fifo levels). Copied.
 * @param out_dev  On success, set to the driver instance pointer.
 */
freertos_uart_status_t freertos_uart_init(uint32_t module,
                                          const am_hal_uart_config_t *cfg,
                                          freertos_uart_dev_t **out_dev);

/**
 * @brief Fully tear down the UART: disable interrupts, power down, free.
 */
freertos_uart_status_t freertos_uart_deinit(freertos_uart_dev_t *dev);

/* -------------------------------------------------------------------------- */
/* Byte / buffer I/O                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Queue bytes for transmission.
 *
 * Blocks in the TX stream buffer if there is no room, up to @p timeout ticks.
 * Returns the number of bytes accepted in @p written.
 */
freertos_uart_status_t freertos_uart_write(freertos_uart_dev_t *dev,
                                           const uint8_t *data,
                                           size_t len,
                                           size_t *written,
                                           TickType_t timeout);

/**
 * @brief Receive bytes from the UART.
 *
 * Returns as soon as at least one byte is available, up to @p len, or after
 * @p timeout ticks elapse. Number of bytes returned is stored in @p got.
 */
freertos_uart_status_t freertos_uart_read(freertos_uart_dev_t *dev,
                                          uint8_t *data,
                                          size_t len,
                                          size_t *got,
                                          TickType_t timeout);

/**
 * @brief Block until every byte currently in the TX pipeline (SW buffer +
 *        hardware FIFO + shift register) has left the pin.
 */
freertos_uart_status_t freertos_uart_flush(freertos_uart_dev_t *dev,
                                           TickType_t timeout);

/**
 * @brief Discard any bytes currently sitting in the SW RX buffer.
 */
freertos_uart_status_t freertos_uart_rx_purge(freertos_uart_dev_t *dev);

/**
 * @brief Reconfigure baud / framing at runtime. Will stall until the TX
 *        buffer has drained.
 */
freertos_uart_status_t freertos_uart_reconfigure(freertos_uart_dev_t *dev,
                                                 const am_hal_uart_config_t *cfg,
                                                 TickType_t timeout);

/* -------------------------------------------------------------------------- */
/* ISR plumbing                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Call from the UARTx_IRQHandler in the application vector table.
 *
 * Example:
 *   void am_uart0_isr(void) { freertos_uart_irq_handler(uart0_dev); }
 */
void freertos_uart_irq_handler(freertos_uart_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_UART_H */
