/*
 * freertos_mspi.h - FreeRTOS wrapper over the Ambiq Apollo510 MSPI HAL
 *
 * This driver provides a thin, thread-safe API on top of the AmbiqSuite
 * low-level MSPI HAL (am_hal_mspi_*). It is intentionally minimal: its only
 * job is to serialize access to a single MSPI module from multiple FreeRTOS
 * tasks and to provide synchronous/asynchronous completion semantics using
 * OS-native primitives (queues, mutexes, task notifications).
 *
 * Design:
 *   - One instance per physical MSPI module.
 *   - A FreeRTOS queue owns outstanding transfer requests. A dedicated worker
 *     task drains the queue and submits requests to the HAL's non-blocking
 *     DMA/CQ engine.
 *   - Completion from the HAL callback (ISR context) is reported back to the
 *     requester via direct-to-task notification - zero-copy, zero-allocation
 *     and the fastest FreeRTOS sync primitive.
 *   - A recursive mutex protects the HAL handle during configuration changes.
 *   - All blocking APIs accept an OS tick timeout; none take the scheduler
 *     offline.
 *
 * The upper-layer device driver (flash, PSRAM, display, ...) is expected to
 * build its own command sequences on top of these primitives.
 */

#ifndef FREERTOS_MSPI_H
#define FREERTOS_MSPI_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "am_mcu_apollo.h"   /* pulls in am_hal_mspi.h */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compile-time configuration                                                 */
/* -------------------------------------------------------------------------- */

#ifndef FREERTOS_MSPI_REQUEST_QUEUE_DEPTH
#define FREERTOS_MSPI_REQUEST_QUEUE_DEPTH       8u
#endif

#ifndef FREERTOS_MSPI_WORKER_STACK_WORDS
#define FREERTOS_MSPI_WORKER_STACK_WORDS        512u
#endif

#ifndef FREERTOS_MSPI_WORKER_PRIORITY
#define FREERTOS_MSPI_WORKER_PRIORITY           (configMAX_PRIORITIES - 2)
#endif

#ifndef FREERTOS_MSPI_DEFAULT_TIMEOUT_MS
#define FREERTOS_MSPI_DEFAULT_TIMEOUT_MS        1000u
#endif

/* TCB (transfer control buffer) used by the HAL command queue. Size is in
 * 32-bit words. 1KB gives comfortable headroom for chained transfers. */
#ifndef FREERTOS_MSPI_TCB_SIZE_WORDS
#define FREERTOS_MSPI_TCB_SIZE_WORDS            256u
#endif

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

/** Driver status codes. Negative = error, zero = success. */
typedef enum {
    FREERTOS_MSPI_OK             =  0,
    FREERTOS_MSPI_ERR_PARAM      = -1,
    FREERTOS_MSPI_ERR_STATE      = -2,   /* not initialized, or busy */
    FREERTOS_MSPI_ERR_HAL        = -3,   /* HAL returned non-success */
    FREERTOS_MSPI_ERR_TIMEOUT    = -4,
    FREERTOS_MSPI_ERR_NOMEM      = -5,
    FREERTOS_MSPI_ERR_ABORTED    = -6,
} freertos_mspi_status_t;

/** Flavor of a single transfer request. */
typedef enum {
    FREERTOS_MSPI_XFER_PIO = 0,          /* short, CPU-synchronous PIO     */
    FREERTOS_MSPI_XFER_DMA,              /* DMA (automatically uses the    */
                                         /* HAL command queue under the    */
                                         /* hood on Apollo510)             */
} freertos_mspi_xfer_kind_t;

/** Optional async completion callback (runs in the worker task context,
 *  NOT in the ISR). Use a sync transfer if you need ISR-context handling. */
typedef void (*freertos_mspi_complete_cb_t)(freertos_mspi_status_t status,
                                            void *user_ctx);

/**
 * A single unit of work submitted to the driver.
 *
 * Either @p pio or @p dma must be populated to match @p kind. The memory
 * pointed to by the transfer (buffer, addresses) must remain valid until
 * completion.
 */
typedef struct {
    freertos_mspi_xfer_kind_t   kind;

    union {
        am_hal_mspi_pio_transfer_t  pio;
        am_hal_mspi_dma_transfer_t  dma;
    } u;

    /* Completion delivery (choose exactly one):
     *   - If async_cb != NULL  -> fire-and-forget, callback invoked later.
     *   - Else                 -> caller blocks in freertos_mspi_submit_sync.
     */
    freertos_mspi_complete_cb_t async_cb;
    void                       *async_ctx;
} freertos_mspi_xfer_t;

/** Opaque driver instance. */
typedef struct freertos_mspi_dev freertos_mspi_dev_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize a FreeRTOS MSPI instance.
 *
 * Takes ownership of the given MSPI module number, calls
 * am_hal_mspi_initialize / power_control / configure / device_configure /
 * enable, installs the interrupt mask, and spawns the worker task.
 *
 * @param module     MSPI module index (0, 1, 2, ...).
 * @param mspi_cfg   General MSPI config (TCB, pads). May be NULL, in which
 *                   case the driver allocates a TCB of
 *                   FREERTOS_MSPI_TCB_SIZE_WORDS.
 * @param dev_cfg    Device-specific config (mode, clock, instr/addr, ...).
 * @param out_dev    On success, receives the instance pointer.
 *
 * @return FREERTOS_MSPI_OK on success, negative status otherwise.
 */
freertos_mspi_status_t freertos_mspi_init(uint32_t module,
                                          const am_hal_mspi_config_t *mspi_cfg,
                                          const am_hal_mspi_dev_config_t *dev_cfg,
                                          freertos_mspi_dev_t **out_dev);

/**
 * @brief Tear down the driver: disables the HAL, stops the worker, frees
 *        resources. Any pending requests complete with FREERTOS_MSPI_ERR_ABORTED.
 */
freertos_mspi_status_t freertos_mspi_deinit(freertos_mspi_dev_t *dev);

/**
 * @brief Re-apply a device configuration at runtime (e.g. switching clock
 *        freq or lane width). Safe to call from any task; will block until
 *        the pipeline is idle.
 */
freertos_mspi_status_t freertos_mspi_reconfigure(freertos_mspi_dev_t *dev,
                                                 const am_hal_mspi_dev_config_t *dev_cfg,
                                                 TickType_t timeout);

/* -------------------------------------------------------------------------- */
/* I/O API                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Submit a transfer and block until it completes or times out.
 *
 * The calling task is suspended using a direct-to-task notification, so there
 * is no busy-wait and no dependence on queue polling.
 */
freertos_mspi_status_t freertos_mspi_submit_sync(freertos_mspi_dev_t *dev,
                                                 freertos_mspi_xfer_t *xfer,
                                                 TickType_t timeout);

/**
 * @brief Submit a transfer asynchronously.
 *
 * The xfer->async_cb (if set) is invoked from the worker task when the HAL
 * reports completion. The @p xfer storage MUST remain valid until then.
 */
freertos_mspi_status_t freertos_mspi_submit_async(freertos_mspi_dev_t *dev,
                                                  freertos_mspi_xfer_t *xfer,
                                                  TickType_t enqueue_timeout);

/**
 * @brief Wait for every queued request to finish (useful for shutdown /
 *        low-power entry).
 */
freertos_mspi_status_t freertos_mspi_drain(freertos_mspi_dev_t *dev,
                                           TickType_t timeout);

/* -------------------------------------------------------------------------- */
/* ISR glue                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Must be called from the MSPIx_IRQHandler in the application's vector
 *        table. This forwards the interrupt into the HAL's service routine,
 *        which in turn fires the completion callback installed by the driver.
 *
 * Example:
 *     void am_mspi0_isr(void) { freertos_mspi_irq_handler(mspi0_dev); }
 */
void freertos_mspi_irq_handler(freertos_mspi_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_MSPI_H */
