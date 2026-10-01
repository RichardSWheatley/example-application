#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "nand_cmd.h"
#include "spi_bus.h"

LOG_MODULE_REGISTER(nand_cmd, LOG_LEVEL_INF);

/* APS51216BA linear burst on the wire (physical mirror of soft NAND). */
#define APS_Z8_LINEAR_BURST_WRITE 0xA0U
#define APS_Z8_LINEAR_BURST_READ  0x20U
#define NAND_PSRAM_BASE           0x00020000U
#define NAND_BUSY_US              50U
#define PAGE_ALIGN_4K             4096U

#define DMA_BUFF __attribute__((section(".ambiq_dma_buff"))) __aligned(PAGE_ALIGN_4K)

static uint8_t cmd_scratch[4] DMA_BUFF;
static uint8_t status_scratch[4] DMA_BUFF;

static uint32_t cycle_ok;
static uint32_t cycle_fail;

static uint32_t page_addr(uint16_t page)
{
	return NAND_PSRAM_BASE + ((uint32_t)page * NAND_CMD_PAGE_SIZE);
}

static uint8_t wire_cmd(bool is_tx)
{
	return is_tx ? APS_Z8_LINEAR_BURST_WRITE : APS_Z8_LINEAR_BURST_READ;
}

int nand_cmd_init(struct nand_cmd_ctx *ctx, uint8_t *page_cache, uint8_t *array)
{
	if (ctx == NULL || page_cache == NULL || array == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->page_cache = page_cache;
	ctx->array = array;
	memset(page_cache, 0xFF, NAND_CMD_PAGE_SIZE);
	memset(array, 0xFF, NAND_CMD_ARRAY_SIZE);
	cycle_ok = 0;
	cycle_fail = 0;

	LOG_INF("NAND cmd layer: %u x %u B, PSRAM mirror @ 0x%08x", NAND_CMD_PAGES,
		NAND_CMD_PAGE_SIZE, NAND_PSRAM_BASE);
	return 0;
}

int nand_cmd_page_program(struct nand_cmd_ctx *ctx, uint16_t page,
			  uint16_t column, const uint8_t *data, size_t len)
{
	int ret;
	int final = 0;

	if (ctx == NULL || data == NULL || page >= NAND_CMD_PAGES ||
	    column >= NAND_CMD_PAGE_SIZE || (column + len) > NAND_CMD_PAGE_SIZE ||
	    len == 0U) {
		return -EINVAL;
	}

	ret = spi_bus_sequence_begin();
	if (ret) {
		return ret;
	}

	ctx->status |= W25N_STATUS_WEL;
	cmd_scratch[0] = W25N_CMD_WREN;
	cmd_scratch[1] = 0;
	cmd_scratch[2] = 0;
	cmd_scratch[3] = 0;
	ret = spi_bus_xfer(false, true, wire_cmd(true), page_addr(page), cmd_scratch,
			   sizeof(cmd_scratch));
	if (ret) {
		final = ret;
		goto out;
	}

	memcpy(ctx->page_cache + column, data, len);
	ctx->cache_page = page;
	ctx->cache_valid = true;

	ret = spi_bus_xfer(true, true, wire_cmd(true), page_addr(page), ctx->page_cache,
			   NAND_CMD_PAGE_SIZE);
	if (ret) {
		final = ret;
		goto out;
	}

	ctx->status |= W25N_STATUS_OIP;
	memcpy(ctx->array + ((size_t)page * NAND_CMD_PAGE_SIZE), ctx->page_cache,
	       NAND_CMD_PAGE_SIZE);
	k_busy_wait(NAND_BUSY_US);
	ctx->status &= (uint8_t)~(W25N_STATUS_OIP | W25N_STATUS_WEL);

	cmd_scratch[0] = W25N_CMD_PROGRAM_EXECUTE;
	cmd_scratch[1] = (uint8_t)(page >> 8);
	cmd_scratch[2] = (uint8_t)page;
	cmd_scratch[3] = 0;
	ret = spi_bus_xfer(false, true, wire_cmd(true), page_addr(page), cmd_scratch,
			   sizeof(cmd_scratch));
	if (ret) {
		final = ret;
		goto out;
	}

	status_scratch[0] = ctx->status;
	status_scratch[1] = W25N_FEAT_STATUS;
	status_scratch[2] = 0;
	status_scratch[3] = 0;
	ret = spi_bus_xfer(false, true, wire_cmd(true), page_addr(page), status_scratch,
			   sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	memset(status_scratch, 0, sizeof(status_scratch));
	ret = spi_bus_xfer(false, false, wire_cmd(false), page_addr(page),
			   status_scratch, sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(status_scratch, sizeof(status_scratch));

out:
	ret = spi_bus_sequence_end();
	if (ret && final == 0) {
		final = ret;
	}
	return final;
}

int nand_cmd_page_read(struct nand_cmd_ctx *ctx, uint16_t page, uint16_t column,
		       uint8_t *data, size_t len)
{
	int ret;
	int final = 0;

	if (ctx == NULL || data == NULL || page >= NAND_CMD_PAGES ||
	    column >= NAND_CMD_PAGE_SIZE || (column + len) > NAND_CMD_PAGE_SIZE ||
	    len == 0U) {
		return -EINVAL;
	}

	ret = spi_bus_sequence_begin();
	if (ret) {
		return ret;
	}

	ctx->status |= W25N_STATUS_OIP;
	memcpy(ctx->page_cache, ctx->array + ((size_t)page * NAND_CMD_PAGE_SIZE),
	       NAND_CMD_PAGE_SIZE);
	k_busy_wait(NAND_BUSY_US);
	ctx->status &= (uint8_t)~W25N_STATUS_OIP;
	ctx->cache_page = page;
	ctx->cache_valid = true;

	cmd_scratch[0] = W25N_CMD_PAGE_READ;
	cmd_scratch[1] = (uint8_t)(page >> 8);
	cmd_scratch[2] = (uint8_t)page;
	cmd_scratch[3] = 0;
	ret = spi_bus_xfer(false, true, wire_cmd(true), page_addr(page), cmd_scratch,
			   sizeof(cmd_scratch));
	if (ret) {
		final = ret;
		goto out;
	}

	status_scratch[0] = ctx->status;
	ret = spi_bus_xfer(false, true, wire_cmd(true), page_addr(page), status_scratch,
			   sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	memset(status_scratch, 0, sizeof(status_scratch));
	ret = spi_bus_xfer(false, false, wire_cmd(false), page_addr(page),
			   status_scratch, sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(status_scratch, sizeof(status_scratch));

	ret = spi_bus_xfer(true, true, wire_cmd(true), page_addr(page), ctx->page_cache,
			   NAND_CMD_PAGE_SIZE);
	if (ret) {
		final = ret;
		goto out;
	}
	ret = spi_bus_xfer(true, false, wire_cmd(false), page_addr(page),
			   ctx->page_cache, NAND_CMD_PAGE_SIZE);
	if (ret) {
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(ctx->page_cache, NAND_CMD_PAGE_SIZE);
	memcpy(data, ctx->page_cache + column, len);

out:
	ret = spi_bus_sequence_end();
	if (ret && final == 0) {
		final = ret;
	}
	return final;
}

int nand_cmd_stress_cycle(struct nand_cmd_ctx *ctx, uint32_t seq, uint8_t *tx,
			  uint8_t *rx)
{
	uint16_t page = (uint16_t)(seq % NAND_CMD_PAGES);
	int ret;

	if (ctx == NULL || tx == NULL || rx == NULL) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < NAND_CMD_PAGE_SIZE; i++) {
		tx[i] = (uint8_t)(seq + i);
	}
	memset(rx, 0, NAND_CMD_PAGE_SIZE);

	ret = nand_cmd_page_program(ctx, page, 0, tx, NAND_CMD_PAGE_SIZE);
	if (ret) {
		cycle_fail++;
		if ((cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u err=%d (program) ok=%u fail=%u",
				seq, ret, cycle_ok, cycle_fail);
		}
		return ret;
	}

	ret = nand_cmd_page_read(ctx, page, 0, rx, NAND_CMD_PAGE_SIZE);
	if (ret) {
		cycle_fail++;
		if ((cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u err=%d (read) ok=%u fail=%u", seq,
				ret, cycle_ok, cycle_fail);
		}
		return ret;
	}

	if (memcmp(tx, rx, NAND_CMD_PAGE_SIZE) != 0) {
		cycle_fail++;
		if ((cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u data mismatch ok=%u fail=%u", seq,
				cycle_ok, cycle_fail);
		}
		return -EIO;
	}

	cycle_ok++;
	if ((cycle_ok & 0x3FU) == 0U) {
		LOG_INF("stress ok seq=%u page=%u ok=%u fail=%u", seq, page, cycle_ok,
			cycle_fail);
	}

	return 0;
}

void nand_cmd_stress_loop(struct nand_cmd_ctx *ctx, uint8_t *tx, uint8_t *rx,
			  uint32_t loop_delay_ms)
{
	uint32_t seq = 0;

	LOG_INF("NAND cmd stress: page program/read via spi_bus");

	while (1) {
		(void)nand_cmd_stress_cycle(ctx, seq, tx, rx);
		seq++;
		k_msleep(loop_delay_ms);
	}
}
