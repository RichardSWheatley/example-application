/*
 * Whoop-shaped MSPI shim — the only app file that includes zephyr/drivers/mspi.h.
 *
 * Transaction path uses four Zephyr MSPI entry points and nothing else:
 *   mspi_dev_config(..., MSPI_DEVICE_CONFIG_NONE, ...)  acquire
 *   mspi_dev_config(..., MSPI_DEVICE_CONFIG_ALL, ...)   mode/clock change
 *   mspi_transceive(...)
 *   mspi_get_channel_status(...)                        release
 *
 * Boot timing scan is Ambiq bring-up in this same file (not part of the
 * four-API transaction contract).
 */

#ifndef SPI_BUS_MSPI_DRIVER_H
#define SPI_BUS_MSPI_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int spi_bus_mspi_driver_init(void);

/* Four-API transaction surface */
int spi_bus_mspi_driver_acquire(void);
int spi_bus_mspi_driver_configure_all(uint32_t freq_hz);
int spi_bus_mspi_driver_transceive(bool use_dma, bool is_tx, uint8_t cmd,
				   uint32_t addr, uint8_t *buf, uint32_t len);
int spi_bus_mspi_driver_release(void);

/* Boot-only Ambiq timing scan (outside the four-API stress contract). */
int spi_bus_mspi_driver_boot_timing_scan(void);

uint32_t spi_bus_mspi_driver_get_freq(void);

#ifdef __cplusplus
}
#endif

#endif /* SPI_BUS_MSPI_DRIVER_H */
