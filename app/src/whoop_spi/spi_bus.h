/*
 * Bus arbitration and transaction sequencing above spi_bus_mspi_driver.
 * Tracks PIO vs DMA and issues CONFIG_ALL when the path changes.
 */

#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int spi_bus_init(void);
int spi_bus_boot_timing_scan(void);

/* Begin a multi-command sequence (CONFIG_NONE acquire). */
int spi_bus_sequence_begin(void);
/* End sequence (get_channel_status release). Always call. */
int spi_bus_sequence_end(void);

/*
 * Single sync transfer. On PIO<->DMA switch, issues CONFIG_ALL first
 * (Whoop: reconfigure when data path / opcode mode changes).
 */
int spi_bus_xfer(bool use_dma, bool is_tx, uint8_t cmd, uint32_t addr,
		 uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* SPI_BUS_H */
