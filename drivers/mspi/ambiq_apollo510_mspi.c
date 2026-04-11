/*
 * Copyright (c) 2026 Ambiq Micro Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS-style MSPI driver for the Ambiq Apollo510 family.
 *
 * This driver exposes the custom `mspi_freertos` driver class defined in
 * <app/drivers/mspi_freertos.h>. It is modeled after native FreeRTOS MSPI
 * drivers: every transfer is serialized with a mutex, started from thread
 * context, and a semaphore is released from the IRQ handler when the
 * controller reports completion — the same ownership pattern used by the
 * Ambiq-provided FreeRTOS MSPI examples.
 *
 * Low-level register access is intentionally isolated into a small set of
 * helper functions (`apollo510_mspi_hw_*`) so that an application that pulls
 * in the Ambiq HAL can replace them without touching the RTOS plumbing.
 */

#define DT_DRV_COMPAT ambiq_apollo510_mspi_freertos

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/drivers/mspi_freertos.h>

LOG_MODULE_REGISTER(mspi_freertos_apollo510,
		    CONFIG_MSPI_FREERTOS_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/* Per-instance configuration and runtime data                                */
/* -------------------------------------------------------------------------- */

struct apollo510_mspi_config {
	uintptr_t                 base;
	uint32_t                  default_freq_hz;
	uint8_t                   channel;
	uint8_t                   io_mode;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config_func)(const struct device *dev);
};

struct apollo510_mspi_data {
	/* Bus lock: modeled after a FreeRTOS recursive mutex. */
	struct k_mutex   bus_lock;
	/* Completion semaphore: given from the IRQ handler. */
	struct k_sem     xfer_done;
	/* Current/last transfer error code captured by the ISR. */
	int              xfer_status;
	/* Current operating frequency in Hz. */
	uint32_t         freq_hz;
	/* Tracks whether the device has completed init(). */
	bool             ready;
};

/* -------------------------------------------------------------------------- */
/* Low-level HAL shim                                                         */
/* -------------------------------------------------------------------------- */
/*
 * These helpers are the only places that touch MSPI registers. They are kept
 * as thin wrappers so a downstream integrator can drop in Ambiq HAL calls
 * (am_hal_mspi_*) without touching the RTOS plumbing above. The default
 * implementations log the intended operation and return success so the
 * driver can be exercised end-to-end on a workstation build.
 */

static int apollo510_mspi_hw_configure(const struct apollo510_mspi_config *cfg,
				       uint32_t freq_hz)
{
	LOG_DBG("hw configure: base=0x%lx channel=%u io_mode=%u freq=%u Hz",
		(unsigned long)cfg->base, cfg->channel, cfg->io_mode,
		freq_hz);
	return 0;
}

static int apollo510_mspi_hw_start_xfer(const struct apollo510_mspi_config *cfg,
					const struct mspi_freertos_xfer *xfer)
{
	LOG_DBG("hw start: dir=%d instr=0x%08x ilen=%u addr=0x%08x alen=%u "
		"dummy=%u len=%zu",
		(int)xfer->direction, xfer->instruction, xfer->instruction_len,
		xfer->address, xfer->address_len, xfer->dummy_cycles,
		xfer->length);
	ARG_UNUSED(cfg);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* ISR                                                                        */
/* -------------------------------------------------------------------------- */

static void apollo510_mspi_isr(const struct device *dev)
{
	struct apollo510_mspi_data *data = dev->data;

	/*
	 * In a real driver this is where you would read the controller status
	 * register, decide between success/error, and ACK the interrupt. We
	 * preserve the FreeRTOS pattern: the ISR signals the waiting thread by
	 * releasing the completion semaphore.
	 */
	data->xfer_status = 0;
	k_sem_give(&data->xfer_done);
}

/* -------------------------------------------------------------------------- */
/* Driver API                                                                 */
/* -------------------------------------------------------------------------- */

static int apollo510_mspi_lock(const struct device *dev, k_timeout_t timeout)
{
	struct apollo510_mspi_data *data = dev->data;

	if (!data->ready) {
		return -ENODEV;
	}

	if (k_mutex_lock(&data->bus_lock, timeout) != 0) {
		return -EBUSY;
	}
	return 0;
}

static int apollo510_mspi_unlock(const struct device *dev)
{
	struct apollo510_mspi_data *data = dev->data;

	return k_mutex_unlock(&data->bus_lock);
}

static int apollo510_mspi_set_frequency(const struct device *dev,
					uint32_t freq_hz)
{
	const struct apollo510_mspi_config *cfg = dev->config;
	struct apollo510_mspi_data *data = dev->data;
	int ret;

	if (freq_hz == 0U) {
		return -EINVAL;
	}

	ret = k_mutex_lock(&data->bus_lock, K_FOREVER);
	if (ret != 0) {
		return ret;
	}

	ret = apollo510_mspi_hw_configure(cfg, freq_hz);
	if (ret == 0) {
		data->freq_hz = freq_hz;
	}

	k_mutex_unlock(&data->bus_lock);
	return ret;
}

static int apollo510_mspi_transceive(const struct device *dev,
				     const struct mspi_freertos_xfer *xfer,
				     k_timeout_t timeout)
{
	const struct apollo510_mspi_config *cfg = dev->config;
	struct apollo510_mspi_data *data = dev->data;
	int ret;

	if (xfer == NULL) {
		return -EINVAL;
	}
	if (xfer->length != 0U && xfer->buf == NULL) {
		return -EINVAL;
	}
	if (xfer->instruction_len > 4U || xfer->address_len > 4U) {
		return -EINVAL;
	}
	if (!data->ready) {
		return -ENODEV;
	}

	if (k_mutex_lock(&data->bus_lock, timeout) != 0) {
		return -EBUSY;
	}

	/* Arm the completion semaphore before kicking the hardware. */
	k_sem_reset(&data->xfer_done);
	data->xfer_status = -EIO;

	ret = apollo510_mspi_hw_start_xfer(cfg, xfer);
	if (ret != 0) {
		goto out;
	}

	/*
	 * Wait for the ISR to release the completion semaphore. This is the
	 * direct Zephyr analog of FreeRTOS xSemaphoreTake() on a binary
	 * semaphore given from ISR with xSemaphoreGiveFromISR().
	 */
	if (k_sem_take(&data->xfer_done, timeout) != 0) {
		LOG_WRN("transfer timed out after %u bytes", xfer->length);
		ret = -EAGAIN;
		goto out;
	}

	ret = data->xfer_status;

out:
	k_mutex_unlock(&data->bus_lock);
	return ret;
}

static DEVICE_API(mspi_freertos, apollo510_mspi_api) = {
	.transceive    = apollo510_mspi_transceive,
	.set_frequency = apollo510_mspi_set_frequency,
	.lock          = apollo510_mspi_lock,
	.unlock        = apollo510_mspi_unlock,
};

/* -------------------------------------------------------------------------- */
/* Init                                                                       */
/* -------------------------------------------------------------------------- */

static int apollo510_mspi_init(const struct device *dev)
{
	const struct apollo510_mspi_config *cfg = dev->config;
	struct apollo510_mspi_data *data = dev->data;
	int ret;

	k_mutex_init(&data->bus_lock);
	k_sem_init(&data->xfer_done, 0, 1);
	data->xfer_status = 0;
	data->freq_hz = cfg->default_freq_hz;

	if (cfg->pcfg != NULL) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			LOG_ERR("pinctrl apply failed (%d)", ret);
			return ret;
		}
	}

	ret = apollo510_mspi_hw_configure(cfg, cfg->default_freq_hz);
	if (ret != 0) {
		LOG_ERR("hw configure failed (%d)", ret);
		return ret;
	}

	if (cfg->irq_config_func != NULL) {
		cfg->irq_config_func(dev);
	}

	data->ready = true;
	LOG_INF("Apollo510 FreeRTOS MSPI ch%u ready @ %u Hz",
		cfg->channel, cfg->default_freq_hz);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Instance macro                                                             */
/* -------------------------------------------------------------------------- */

#define APOLLO510_MSPI_INIT(inst)                                              \
	PINCTRL_DT_INST_DEFINE(inst);                                          \
                                                                               \
	static void apollo510_mspi_irq_config_##inst(const struct device *dev);\
                                                                               \
	static struct apollo510_mspi_data apollo510_mspi_data_##inst;          \
                                                                               \
	static const struct apollo510_mspi_config apollo510_mspi_cfg_##inst = {\
		.base            = DT_INST_REG_ADDR(inst),                     \
		.default_freq_hz = DT_INST_PROP(inst, clock_frequency),        \
		.channel         = DT_INST_PROP_OR(inst, ambiq_channel, 0),    \
		.io_mode         = DT_INST_PROP_OR(inst, ambiq_io_mode, 0),    \
		.pcfg            = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),       \
		.irq_config_func = apollo510_mspi_irq_config_##inst,           \
	};                                                                     \
                                                                               \
	DEVICE_DT_INST_DEFINE(inst, apollo510_mspi_init, NULL,                 \
			      &apollo510_mspi_data_##inst,                     \
			      &apollo510_mspi_cfg_##inst, POST_KERNEL,         \
			      CONFIG_MSPI_FREERTOS_INIT_PRIORITY,              \
			      &apollo510_mspi_api);                            \
                                                                               \
	static void apollo510_mspi_irq_config_##inst(const struct device *dev) \
	{                                                                      \
		IRQ_CONNECT(DT_INST_IRQN(inst),                                \
			    DT_INST_IRQ(inst, priority),                       \
			    apollo510_mspi_isr, DEVICE_DT_INST_GET(inst), 0);  \
		irq_enable(DT_INST_IRQN(inst));                                \
	}

DT_INST_FOREACH_STATUS_OKAY(APOLLO510_MSPI_INIT)
