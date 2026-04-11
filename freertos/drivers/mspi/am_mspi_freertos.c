/*
 * Copyright (c) 2026 Ambiq Micro Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS MSPI driver for the Ambiq Apollo510 family.
 *
 * See am_mspi_freertos.h for the public API. This implementation drives the
 * Apollo5 HAL am_hal_mspi_* API directly and uses the standard FreeRTOS
 * producer/consumer pattern: a recursive mutex guards the bus, and the ISR
 * releases a binary semaphore via xSemaphoreGiveFromISR() to wake the
 * calling task.
 */

#include "am_mspi_freertos.h"

#include <errno.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "am_mcu_apollo.h"

/* -------------------------------------------------------------------------- */
/* Instance table                                                             */
/* -------------------------------------------------------------------------- */

static am_mspi_freertos_t *s_instances[AM_MSPI_FREERTOS_NUM_MODULES];

/* -------------------------------------------------------------------------- */
/* HAL completion callback                                                    */
/* -------------------------------------------------------------------------- */

static void am_mspi_freertos_hal_cb(void *ctx, uint32_t status)
{
	am_mspi_freertos_t *drv = (am_mspi_freertos_t *)ctx;
	BaseType_t higher_prio_woken = pdFALSE;

	if (drv == NULL) {
		return;
	}

	drv->last_status = (status == AM_HAL_STATUS_SUCCESS) ? 0 : -EIO;
	drv->xfer_active = false;

	/*
	 * The HAL callback runs from interrupt context on Apollo parts, so we
	 * must use the *FromISR variant and request a context switch on exit.
	 */
	(void)xSemaphoreGiveFromISR(drv->done_sem, &higher_prio_woken);
	portYIELD_FROM_ISR(higher_prio_woken);
}

/* -------------------------------------------------------------------------- */
/* ISR plumbing                                                               */
/* -------------------------------------------------------------------------- */

void am_mspi_freertos_irq_handler(uint32_t module)
{
	if (module >= AM_MSPI_FREERTOS_NUM_MODULES) {
		return;
	}

	am_mspi_freertos_t *drv = s_instances[module];

	if (drv == NULL || drv->hal_handle == NULL) {
		return;
	}

	uint32_t ui32Status = 0U;

	am_hal_mspi_interrupt_status_get(drv->hal_handle, &ui32Status, false);
	am_hal_mspi_interrupt_clear(drv->hal_handle, ui32Status);
	am_hal_mspi_interrupt_service(drv->hal_handle, ui32Status);
	/*
	 * The HAL service routine will invoke am_mspi_freertos_hal_cb() for
	 * any completed transfer, which in turn releases the semaphore and
	 * requests a context switch via portYIELD_FROM_ISR().
	 */
}

/*
 * Weak ISR shims. If the application provides its own vector table entries
 * it can override these; otherwise link-time these become the MSPIn_IRQHandler
 * symbols referenced by the Ambiq startup code.
 */
void __attribute__((weak)) am_mspi0_isr(void)
{
	am_mspi_freertos_irq_handler(0U);
}
void __attribute__((weak)) am_mspi1_isr(void)
{
	am_mspi_freertos_irq_handler(1U);
}
void __attribute__((weak)) am_mspi2_isr(void)
{
	am_mspi_freertos_irq_handler(2U);
}
void __attribute__((weak)) am_mspi3_isr(void)
{
	am_mspi_freertos_irq_handler(3U);
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

static const IRQn_Type s_mspi_irqn[AM_MSPI_FREERTOS_NUM_MODULES] = {
	MSPI0_IRQn, MSPI1_IRQn, MSPI2_IRQn, MSPI3_IRQn,
};

int am_mspi_freertos_init(am_mspi_freertos_t *drv,
			  const am_mspi_freertos_config_t *cfg)
{
	if (drv == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->module >= AM_MSPI_FREERTOS_NUM_MODULES) {
		return -EINVAL;
	}
	if (s_instances[cfg->module] != NULL) {
		return -EBUSY;
	}

	memset(drv, 0, sizeof(*drv));
	drv->module          = cfg->module;
	drv->default_freq_hz = cfg->default_freq_hz;

	/*
	 * Static allocation keeps the driver usable from contexts where heap
	 * use is forbidden (configSUPPORT_DYNAMIC_ALLOCATION may be 0).
	 */
	drv->bus_mutex = xSemaphoreCreateRecursiveMutexStatic(&drv->bus_mutex_buf);
	drv->done_sem  = xSemaphoreCreateBinaryStatic(&drv->done_sem_buf);
	if (drv->bus_mutex == NULL || drv->done_sem == NULL) {
		return -ENOMEM;
	}

	if (am_hal_mspi_initialize(cfg->module, &drv->hal_handle) !=
	    AM_HAL_STATUS_SUCCESS) {
		return -EIO;
	}

	if (am_hal_mspi_power_control(drv->hal_handle, AM_HAL_SYSCTRL_WAKE,
				      false) != AM_HAL_STATUS_SUCCESS) {
		am_hal_mspi_deinitialize(drv->hal_handle);
		return -EIO;
	}

	am_hal_mspi_dev_config_t sDevCfg = {
		.eDeviceConfig = cfg->device_cfg,
		.eClockFreq    = cfg->clock,
		.eXipMixedMode = cfg->xip_mixed,
	};

	if (am_hal_mspi_device_configure(drv->hal_handle, &sDevCfg) !=
	    AM_HAL_STATUS_SUCCESS) {
		am_hal_mspi_deinitialize(drv->hal_handle);
		return -EIO;
	}

	if (am_hal_mspi_enable(drv->hal_handle) != AM_HAL_STATUS_SUCCESS) {
		am_hal_mspi_deinitialize(drv->hal_handle);
		return -EIO;
	}

	/* Enable only the interrupts the non-blocking API needs. */
	am_hal_mspi_interrupt_clear(drv->hal_handle, AM_HAL_MSPI_INT_CQUPD |
						      AM_HAL_MSPI_INT_ERR);
	am_hal_mspi_interrupt_enable(drv->hal_handle, AM_HAL_MSPI_INT_CQUPD |
						      AM_HAL_MSPI_INT_ERR);

	NVIC_SetPriority(s_mspi_irqn[cfg->module], cfg->irq_priority);
	NVIC_ClearPendingIRQ(s_mspi_irqn[cfg->module]);
	NVIC_EnableIRQ(s_mspi_irqn[cfg->module]);

	s_instances[cfg->module] = drv;
	drv->initialized = true;
	return 0;
}

int am_mspi_freertos_deinit(am_mspi_freertos_t *drv)
{
	if (drv == NULL || !drv->initialized) {
		return -EINVAL;
	}

	NVIC_DisableIRQ(s_mspi_irqn[drv->module]);
	am_hal_mspi_interrupt_disable(drv->hal_handle, 0xFFFFFFFFU);
	am_hal_mspi_disable(drv->hal_handle);
	am_hal_mspi_deinitialize(drv->hal_handle);

	s_instances[drv->module] = NULL;
	drv->hal_handle = NULL;
	drv->initialized = false;
	/*
	 * Static semaphores don't need to be deleted, but doing so makes the
	 * driver re-init-safe by resetting their internal state.
	 */
	vSemaphoreDelete(drv->bus_mutex);
	vSemaphoreDelete(drv->done_sem);
	drv->bus_mutex = NULL;
	drv->done_sem = NULL;
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Bus ownership                                                              */
/* -------------------------------------------------------------------------- */

int am_mspi_freertos_lock(am_mspi_freertos_t *drv, TickType_t timeout_ticks)
{
	if (drv == NULL || !drv->initialized) {
		return -EINVAL;
	}
	if (timeout_ticks == 0U) {
		timeout_ticks = AM_MSPI_FREERTOS_DEFAULT_WAIT;
	}
	if (xSemaphoreTakeRecursive(drv->bus_mutex, timeout_ticks) != pdTRUE) {
		return -EBUSY;
	}
	return 0;
}

int am_mspi_freertos_unlock(am_mspi_freertos_t *drv)
{
	if (drv == NULL || !drv->initialized) {
		return -EINVAL;
	}
	if (xSemaphoreGiveRecursive(drv->bus_mutex) != pdTRUE) {
		return -EPERM;
	}
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Transfers                                                                  */
/* -------------------------------------------------------------------------- */

static int am_mspi_freertos_do_xfer(am_mspi_freertos_t *drv,
				    const am_mspi_freertos_xfer_t *xfer,
				    TickType_t timeout_ticks)
{
	am_hal_mspi_callback_t pfn = am_mspi_freertos_hal_cb;
	am_hal_mspi_dma_transfer_t sTxn;

	memset(&sTxn, 0, sizeof(sTxn));
	sTxn.ui8Priority          = 1;
	sTxn.eDirection           = (xfer->direction == AM_MSPI_FREERTOS_DIR_READ)
					    ? AM_HAL_MSPI_RX
					    : AM_HAL_MSPI_TX;
	sTxn.ui32TransferCount    = xfer->length;
	sTxn.ui32DeviceAddress    = xfer->address;
	sTxn.ui32SRAMAddress      = (uint32_t)(uintptr_t)xfer->buf;
	sTxn.ui32PauseCondition   = 0;
	sTxn.ui32StatusSetClr     = 0;

	/*
	 * Ensure the caller's buffer is visible to DMA before we kick the
	 * transaction. This is a no-op on cache-less builds.
	 */
	if (xfer->direction == AM_MSPI_FREERTOS_DIR_WRITE && xfer->length) {
		am_hal_cachectrl_range_t range = {
			.ui32StartAddr = (uint32_t)(uintptr_t)xfer->buf,
			.ui32Size      = xfer->length,
		};
		am_hal_cachectrl_dcache_clean(&range);
	}

	xSemaphoreTake(drv->done_sem, 0);   /* clear any stale token */
	drv->last_status = -EIO;
	drv->xfer_active = true;

	if (am_hal_mspi_nonblocking_transfer(drv->hal_handle, &sTxn,
					     AM_HAL_MSPI_TRANS_DMA, pfn, drv) !=
	    AM_HAL_STATUS_SUCCESS) {
		drv->xfer_active = false;
		return -EIO;
	}

	if (xSemaphoreTake(drv->done_sem, timeout_ticks) != pdTRUE) {
		drv->xfer_active = false;
		return -ETIMEDOUT;
	}

	if (xfer->direction == AM_MSPI_FREERTOS_DIR_READ && xfer->length) {
		am_hal_cachectrl_range_t range = {
			.ui32StartAddr = (uint32_t)(uintptr_t)xfer->buf,
			.ui32Size      = xfer->length,
		};
		am_hal_cachectrl_dcache_invalidate(&range, false);
	}

	return (int)drv->last_status;
}

int am_mspi_freertos_transceive(am_mspi_freertos_t *drv,
				const am_mspi_freertos_xfer_t *xfer,
				TickType_t timeout_ticks)
{
	if (drv == NULL || !drv->initialized || xfer == NULL) {
		return -EINVAL;
	}
	if (xfer->length != 0U && xfer->buf == NULL) {
		return -EINVAL;
	}
	if (xfer->address_len > 4U || xfer->instruction_len > 2U) {
		return -EINVAL;
	}
	if (timeout_ticks == 0U) {
		timeout_ticks = AM_MSPI_FREERTOS_DEFAULT_WAIT;
	}

	if (xSemaphoreTakeRecursive(drv->bus_mutex, timeout_ticks) != pdTRUE) {
		return -EBUSY;
	}

	int ret = am_mspi_freertos_do_xfer(drv, xfer, timeout_ticks);

	xSemaphoreGiveRecursive(drv->bus_mutex);
	return ret;
}

int am_mspi_freertos_write(am_mspi_freertos_t *drv,
			   uint32_t instruction,
			   uint32_t address,
			   const void *buf,
			   uint32_t length,
			   TickType_t timeout_ticks)
{
	am_mspi_freertos_xfer_t xfer = {
		.direction       = AM_MSPI_FREERTOS_DIR_WRITE,
		.instruction     = instruction,
		.instruction_len = (instruction != 0U) ? 1U : 0U,
		.address         = address,
		.address_len     = 4U,
		.dummy_cycles    = 0U,
		.continue_xfer   = false,
		.buf             = (void *)buf,
		.length          = length,
	};

	return am_mspi_freertos_transceive(drv, &xfer, timeout_ticks);
}

int am_mspi_freertos_read(am_mspi_freertos_t *drv,
			  uint32_t instruction,
			  uint32_t address,
			  void *buf,
			  uint32_t length,
			  TickType_t timeout_ticks)
{
	am_mspi_freertos_xfer_t xfer = {
		.direction       = AM_MSPI_FREERTOS_DIR_READ,
		.instruction     = instruction,
		.instruction_len = (instruction != 0U) ? 1U : 0U,
		.address         = address,
		.address_len     = 4U,
		.dummy_cycles    = 0U,
		.continue_xfer   = false,
		.buf             = buf,
		.length          = length,
	};

	return am_mspi_freertos_transceive(drv, &xfer, timeout_ticks);
}
