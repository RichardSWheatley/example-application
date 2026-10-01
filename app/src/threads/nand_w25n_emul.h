/*
 * Software W25N01GV-shaped serial NAND for Whoop AS-2281 MSPI stress.
 *
 * No Flash 5 Click on apollo510b_mini — this emulates the page op shape
 * (WREN → PROGRAM_LOAD → PROGRAM_EXECUTE → status poll, and PAGE_READ →
 * status → READ_CACHE) while physical traffic uses APS51216BA linear burst
 * so the Ambiq MSPI driver path is still exercised.
 */

#ifndef NAND_W25N_EMUL_H
#define NAND_W25N_EMUL_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#include "mspi_ambiq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Winbond W25N01GV geometry (subset). */
#define NAND_EMUL_PAGE_SIZE     2048U
#define NAND_EMUL_PAGES         8U
#define NAND_EMUL_ARRAY_SIZE    (NAND_EMUL_PAGE_SIZE * NAND_EMUL_PAGES)

/* W25N opcodes we sequence in software. */
#define W25N_CMD_WREN           0x06U
#define W25N_CMD_GET_FEATURE    0x0FU
#define W25N_CMD_SET_FEATURE    0x1FU
#define W25N_CMD_PAGE_READ      0x13U
#define W25N_CMD_READ_CACHE     0x03U
#define W25N_CMD_PROGRAM_LOAD   0x02U
#define W25N_CMD_PROGRAM_EXECUTE 0x10U

#define W25N_FEAT_STATUS        0xC0U
#define W25N_STATUS_OIP         BIT(0)
#define W25N_STATUS_WEL         BIT(1)

struct nand_emul_ctx {
	const struct device *bus;
	struct mspi_dev_id *dev_id;
	const struct mspi_dev_cfg *dev_cfg;
	struct mspi_ambiq_timing_cfg *timing;
	uint32_t timing_mask;
	bool timing_valid;
	/* Soft chip state */
	uint8_t status;
	uint8_t *page_cache;
	uint8_t *array;
	uint16_t cache_page;
	bool cache_valid;
};

int nand_emul_init(struct nand_emul_ctx *ctx, const struct device *bus,
		   struct mspi_dev_id *dev_id, const struct mspi_dev_cfg *dev_cfg,
		   struct mspi_ambiq_timing_cfg *timing, uint32_t timing_mask,
		   bool timing_valid, uint8_t *page_cache, uint8_t *array);

/*
 * One held CONFIG_NONE claim:
 *   PIO WREN → DMA PROGRAM_LOAD → PIO PROGRAM_EXECUTE → PIO GET_FEATURE
 * Always releases via get_channel_status.
 */
int nand_emul_page_program(struct nand_emul_ctx *ctx, uint16_t page,
			   uint16_t column, const uint8_t *data, size_t len);

/*
 * One held CONFIG_NONE claim:
 *   PIO PAGE_READ → PIO GET_FEATURE → DMA READ_CACHE
 * Always releases via get_channel_status.
 */
int nand_emul_page_read(struct nand_emul_ctx *ctx, uint16_t page,
			uint16_t column, uint8_t *data, size_t len);

/*
 * One MSPI stress unit: program page + read-back verify through the NAND
 * emulator (Whoop claim/PIO/DMA/release). This is what MSPI stress runs.
 */
int nand_emul_stress_cycle(struct nand_emul_ctx *ctx, uint32_t seq,
			   uint8_t *tx, uint8_t *rx);

/* Forever loop of nand_emul_stress_cycle(); logs from nand_emul. */
void nand_emul_stress_loop(struct nand_emul_ctx *ctx, uint8_t *tx, uint8_t *rx,
			   uint32_t loop_delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* NAND_W25N_EMUL_H */
