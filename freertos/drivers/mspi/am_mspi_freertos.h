/*
 * Copyright (c) 2026 Ambiq Micro Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS MSPI driver for the Ambiq Apollo510 family.
 *
 * Native FreeRTOS wrapper around the Ambiq HAL MSPI peripheral. Each of the
 * four Apollo510 MSPI controllers is owned by one driver instance that
 * serializes all transfers with a recursive mutex and blocks the calling
 * task on a binary semaphore that the ISR releases via
 * xSemaphoreGiveFromISR(). This is the canonical producer/consumer pattern
 * used by Ambiq's FreeRTOS MSPI reference drivers.
 *
 * This file has no dependency on Zephyr — it only needs FreeRTOS and the
 * Ambiq Apollo5 HAL headers.
 */

#ifndef AM_MSPI_FREERTOS_H
#define AM_MSPI_FREERTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "am_mcu_apollo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Apollo510 exposes four independent MSPI controllers. */
#define AM_MSPI_FREERTOS_NUM_MODULES   4U

/** Default wait time when callers pass 0 to indicate "use the driver default". */
#define AM_MSPI_FREERTOS_DEFAULT_WAIT  pdMS_TO_TICKS(1000)

/** Transfer direction. */
typedef enum {
	AM_MSPI_FREERTOS_DIR_READ  = 0,
	AM_MSPI_FREERTOS_DIR_WRITE = 1,
} am_mspi_freertos_dir_t;

/**
 * @brief Single MSPI transfer descriptor.
 *
 * Models one command/address/data phase. The HAL transaction structure is
 * filled in from these fields before being handed to
 * am_hal_mspi_nonblocking_transfer().
 */
typedef struct {
	am_mspi_freertos_dir_t direction;
	uint32_t               instruction;     /**< Instruction/command value.   */
	uint8_t                instruction_len; /**< 0..2 (MSPI limit).           */
	uint32_t               address;         /**< Device address.              */
	uint8_t                address_len;     /**< 0..4.                        */
	uint8_t                dummy_cycles;    /**< Turnaround after address.    */
	bool                   continue_xfer;   /**< Chain to next without CS release. */
	void                  *buf;             /**< TX/RX data buffer.           */
	uint32_t               length;          /**< Bytes in @p buf.             */
} am_mspi_freertos_xfer_t;

/**
 * @brief Driver handle.
 *
 * Opaque to callers; declared here so the user can statically allocate
 * instances rather than relying on a driver-local pool.
 */
typedef struct am_mspi_freertos {
	uint32_t             module;        /**< 0..3.                           */
	void                *hal_handle;    /**< Opaque HAL handle.              */
	SemaphoreHandle_t    bus_mutex;     /**< Recursive: serializes the bus.  */
	SemaphoreHandle_t    done_sem;      /**< Binary: given from ISR.         */
	StaticSemaphore_t    bus_mutex_buf; /**< Static storage for bus_mutex.   */
	StaticSemaphore_t    done_sem_buf;  /**< Static storage for done_sem.    */
	volatile int32_t     last_status;   /**< Captured in the HAL callback.   */
	volatile bool        xfer_active;
	uint32_t             default_freq_hz;
	bool                 initialized;
} am_mspi_freertos_t;

/**
 * @brief Driver configuration.
 *
 * Populated by the caller before am_mspi_freertos_init(). Everything that
 * cannot change at runtime goes here; per-transfer attributes live in
 * am_mspi_freertos_xfer_t.
 */
typedef struct {
	uint32_t                    module;       /**< 0..3.                  */
	am_hal_mspi_device_e        device_cfg;   /**< HAL device mode enum.  */
	am_hal_mspi_clock_e         clock;        /**< HAL clock selector.    */
	am_hal_mspi_xipmixed_mode_e xip_mixed;    /**< XIP mixing mode.       */
	uint32_t                    default_freq_hz;
	uint8_t                     irq_priority; /**< NVIC priority for MSPIn_IRQ. */
} am_mspi_freertos_config_t;

/* ---- Lifecycle ---------------------------------------------------------- */

/**
 * @brief Initialize an MSPI instance.
 *
 * Creates the bus mutex and completion semaphore, calls
 * am_hal_mspi_initialize()/configure()/enable(), installs the ISR and wires
 * it into the NVIC. Must be called from task context (after the scheduler is
 * running) because it creates FreeRTOS primitives.
 *
 * @return 0 on success, negative errno on failure.
 */
int am_mspi_freertos_init(am_mspi_freertos_t *drv,
			  const am_mspi_freertos_config_t *cfg);

/** Shut down the instance and release its HAL handle. */
int am_mspi_freertos_deinit(am_mspi_freertos_t *drv);

/* ---- Transfers ---------------------------------------------------------- */

/**
 * @brief Execute a single blocking transfer.
 *
 * Acquires the bus mutex, programs the HAL, kicks a non-blocking transfer,
 * and waits on the completion semaphore. Safe to call from any task.
 *
 * @param timeout_ticks Maximum time (in FreeRTOS ticks) to wait for both
 *                      the bus and for completion. Pass portMAX_DELAY to
 *                      block indefinitely.
 *
 * @retval 0        Success.
 * @retval -EBUSY   Bus could not be acquired within @p timeout_ticks.
 * @retval -EIO     HAL reported an error.
 * @retval -ETIMEDOUT Transfer did not complete within the timeout.
 * @retval -EINVAL  @p drv or @p xfer is NULL or @p xfer is malformed.
 */
int am_mspi_freertos_transceive(am_mspi_freertos_t *drv,
				const am_mspi_freertos_xfer_t *xfer,
				TickType_t timeout_ticks);

/** Convenience wrapper for write-only transfers. */
int am_mspi_freertos_write(am_mspi_freertos_t *drv,
			   uint32_t instruction,
			   uint32_t address,
			   const void *buf,
			   uint32_t length,
			   TickType_t timeout_ticks);

/** Convenience wrapper for read-only transfers. */
int am_mspi_freertos_read(am_mspi_freertos_t *drv,
			  uint32_t instruction,
			  uint32_t address,
			  void *buf,
			  uint32_t length,
			  TickType_t timeout_ticks);

/* ---- Bus ownership (for multi-xfer sequences) --------------------------- */

/** Acquire the bus mutex explicitly. Returns 0 or -EBUSY. */
int am_mspi_freertos_lock(am_mspi_freertos_t *drv, TickType_t timeout_ticks);

/** Release the bus mutex previously taken with am_mspi_freertos_lock(). */
int am_mspi_freertos_unlock(am_mspi_freertos_t *drv);

/* ---- ISR plumbing ------------------------------------------------------- */

/**
 * @brief Shared ISR body.
 *
 * Each MSPIn_IRQHandler in the vector table must forward into this function
 * so the driver can look up the right instance, service the HAL, and release
 * the completion semaphore with a context switch request on return.
 */
void am_mspi_freertos_irq_handler(uint32_t module);

#ifdef __cplusplus
}
#endif

#endif /* AM_MSPI_FREERTOS_H */
