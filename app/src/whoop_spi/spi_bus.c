#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "spi_bus.h"
#include "spi_bus_mspi_driver.h"

LOG_MODULE_REGISTER(spi_bus, LOG_LEVEL_INF);

static bool seq_active;
static bool have_last_dma;
static bool last_use_dma;

int spi_bus_init(void)
{
	seq_active = false;
	have_last_dma = false;
	return spi_bus_mspi_driver_init();
}

int spi_bus_boot_timing_scan(void)
{
	return spi_bus_mspi_driver_boot_timing_scan();
}

int spi_bus_sequence_begin(void)
{
	int ret;

	if (seq_active) {
		return -EBUSY;
	}

	ret = spi_bus_mspi_driver_acquire();
	if (ret) {
		return ret;
	}

	seq_active = true;
	have_last_dma = false;
	return 0;
}

int spi_bus_sequence_end(void)
{
	int ret;

	if (!seq_active) {
		return spi_bus_mspi_driver_release();
	}

	ret = spi_bus_mspi_driver_release();
	seq_active = false;
	have_last_dma = false;
	return ret;
}

int spi_bus_xfer(bool use_dma, bool is_tx, uint8_t cmd, uint32_t addr,
		 uint8_t *buf, uint32_t len)
{
	int ret;

	if (!seq_active) {
		return -EPERM;
	}

	if (!have_last_dma || (last_use_dma != use_dma)) {
		ret = spi_bus_mspi_driver_configure_all(
			spi_bus_mspi_driver_get_freq());
		if (ret) {
			LOG_ERR("CONFIG_ALL failed (err %d) dma=%d", ret, use_dma);
			return ret;
		}
		last_use_dma = use_dma;
		have_last_dma = true;
	}

	return spi_bus_mspi_driver_transceive(use_dma, is_tx, cmd, addr, buf, len);
}
