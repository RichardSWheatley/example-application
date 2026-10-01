/*
 * NAND command layer — raw W25N opcodes over spi_bus (Whoop layering).
 * Soft chip state + PSRAM mirror traffic; no zephyr/drivers/mspi.h here.
 */

#ifndef NAND_CMD_H
#define NAND_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAND_CMD_PAGE_SIZE  2048U
#define NAND_CMD_PAGES      8U
#define NAND_CMD_ARRAY_SIZE (NAND_CMD_PAGE_SIZE * NAND_CMD_PAGES)

#define W25N_CMD_WREN            0x06U
#define W25N_CMD_GET_FEATURE     0x0FU
#define W25N_CMD_PAGE_READ       0x13U
#define W25N_CMD_READ_CACHE      0x03U
#define W25N_CMD_PROGRAM_LOAD    0x02U
#define W25N_CMD_PROGRAM_EXECUTE 0x10U

#define W25N_FEAT_STATUS 0xC0U
#define W25N_STATUS_OIP  0x01U
#define W25N_STATUS_WEL  0x02U

struct nand_cmd_ctx {
	uint8_t status;
	uint8_t *page_cache;
	uint8_t *array;
	uint16_t cache_page;
	bool cache_valid;
};

int nand_cmd_init(struct nand_cmd_ctx *ctx, uint8_t *page_cache, uint8_t *array);

/* One held spi_bus sequence: WREN → PROGRAM_LOAD → EXECUTE → GET_FEATURE */
int nand_cmd_page_program(struct nand_cmd_ctx *ctx, uint16_t page,
			  uint16_t column, const uint8_t *data, size_t len);

/* One held spi_bus sequence: PAGE_READ → GET_FEATURE → READ_CACHE */
int nand_cmd_page_read(struct nand_cmd_ctx *ctx, uint16_t page, uint16_t column,
		       uint8_t *data, size_t len);

int nand_cmd_stress_cycle(struct nand_cmd_ctx *ctx, uint32_t seq, uint8_t *tx,
			  uint8_t *rx);
void nand_cmd_stress_loop(struct nand_cmd_ctx *ctx, uint8_t *tx, uint8_t *rx,
			  uint32_t loop_delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* NAND_CMD_H */
