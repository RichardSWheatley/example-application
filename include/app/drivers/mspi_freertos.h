/*
 * Copyright (c) 2026 Ambiq Micro Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DRIVERS_MSPI_FREERTOS_H_
#define APP_DRIVERS_MSPI_FREERTOS_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup drivers_mspi_freertos FreeRTOS MSPI drivers
 * @ingroup drivers
 * @{
 *
 * @brief A custom driver class that exposes a FreeRTOS-style, thread-safe
 * MSPI (Multi-bit SPI) interface for Ambiq Apollo510-class SoCs.
 *
 * This class is intended for applications that need to share a single MSPI
 * controller between multiple RTOS threads while still benefiting from
 * blocking, interrupt-driven transfers. Each operation acquires a bus mutex
 * (modeled after FreeRTOS recursive mutex semantics) and waits on a
 * completion semaphore that is released from the IRQ handler — directly
 * mirroring the `xSemaphoreGiveFromISR()` pattern used in native FreeRTOS
 * drivers.
 */

/** @brief Transfer direction. */
enum mspi_freertos_direction {
	MSPI_FREERTOS_DIR_READ = 0,
	MSPI_FREERTOS_DIR_WRITE = 1,
};

/**
 * @brief Single MSPI transfer descriptor.
 *
 * A transfer optionally starts with a 1–4 byte instruction, followed by an
 * optional 0–4 byte address and an optional number of dummy cycles, and then
 * moves @p length bytes in the direction indicated by @p direction.
 */
struct mspi_freertos_xfer {
	/** Transfer direction. */
	enum mspi_freertos_direction direction;
	/** Instruction/command value sent before the address phase. */
	uint32_t instruction;
	/** Number of instruction bytes (0..4). */
	uint8_t  instruction_len;
	/** Address value. Ignored when @p address_len is 0. */
	uint32_t address;
	/** Number of address bytes (0..4). */
	uint8_t  address_len;
	/** Number of dummy clock cycles to insert after the address phase. */
	uint8_t  dummy_cycles;
	/** Data buffer (TX for WRITE, RX for READ). */
	void    *buf;
	/** Number of data bytes to transfer. */
	size_t   length;
};

/** @brief FreeRTOS MSPI driver class operations. */
__subsystem struct mspi_freertos_driver_api {
	int (*transceive)(const struct device *dev,
			  const struct mspi_freertos_xfer *xfer,
			  k_timeout_t timeout);
	int (*set_frequency)(const struct device *dev, uint32_t freq_hz);
	int (*lock)(const struct device *dev, k_timeout_t timeout);
	int (*unlock)(const struct device *dev);
};

/**
 * @brief Perform a single MSPI transfer.
 *
 * Acquires the bus, issues the transfer described by @p xfer, and blocks the
 * calling thread on a completion semaphore until the IRQ signals completion
 * or @p timeout elapses. Safe to call from any thread context.
 *
 * @param dev     MSPI device instance.
 * @param xfer    Transfer descriptor.
 * @param timeout Maximum time to wait for the bus and for completion.
 *
 * @retval 0        On success.
 * @retval -EBUSY   Bus could not be acquired within @p timeout.
 * @retval -EIO     Hardware reported a transfer error.
 * @retval -EAGAIN  Transfer did not complete within @p timeout.
 * @retval -EINVAL  Invalid argument in @p xfer.
 */
__syscall int mspi_freertos_transceive(const struct device *dev,
				       const struct mspi_freertos_xfer *xfer,
				       k_timeout_t timeout);

static inline int z_impl_mspi_freertos_transceive(
	const struct device *dev,
	const struct mspi_freertos_xfer *xfer,
	k_timeout_t timeout)
{
	const struct mspi_freertos_driver_api *api =
		(const struct mspi_freertos_driver_api *)dev->api;

	return api->transceive(dev, xfer, timeout);
}

/**
 * @brief Change the MSPI bus clock frequency.
 *
 * @param dev     MSPI device instance.
 * @param freq_hz Target SCLK frequency in Hz.
 *
 * @retval 0       On success.
 * @retval -EINVAL Requested frequency is not supported.
 */
__syscall int mspi_freertos_set_frequency(const struct device *dev,
					  uint32_t freq_hz);

static inline int z_impl_mspi_freertos_set_frequency(const struct device *dev,
						     uint32_t freq_hz)
{
	const struct mspi_freertos_driver_api *api =
		(const struct mspi_freertos_driver_api *)dev->api;

	return api->set_frequency(dev, freq_hz);
}

/**
 * @brief Explicitly acquire the bus mutex.
 *
 * Normally transceive() handles locking internally. Use this pair when the
 * caller needs to issue multiple back-to-back transfers without yielding the
 * bus (e.g. erase/program sequences on a flash device).
 */
__syscall int mspi_freertos_lock(const struct device *dev,
				 k_timeout_t timeout);

static inline int z_impl_mspi_freertos_lock(const struct device *dev,
					    k_timeout_t timeout)
{
	const struct mspi_freertos_driver_api *api =
		(const struct mspi_freertos_driver_api *)dev->api;

	return api->lock(dev, timeout);
}

/** @brief Release the bus mutex previously taken with mspi_freertos_lock(). */
__syscall int mspi_freertos_unlock(const struct device *dev);

static inline int z_impl_mspi_freertos_unlock(const struct device *dev)
{
	const struct mspi_freertos_driver_api *api =
		(const struct mspi_freertos_driver_api *)dev->api;

	return api->unlock(dev);
}

/** @} */

#ifdef __cplusplus
}
#endif

#include <syscalls/mspi_freertos.h>

#endif /* APP_DRIVERS_MSPI_FREERTOS_H_ */
