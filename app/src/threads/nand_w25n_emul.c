/*
 * W25N-shaped NAND emulation backed by soft registers + PSRAM mirror traffic.
 *
 * Soft model owns page cache / array / status (OIP, WEL).
 * Each opcode step under a held claim also issues a real Ambiq MSPI
 * PIO/DMA transfer against APS51216BA so CONFIG_NONE + DMA width=4 path
 * is stressed the same way Whoop described for serial NAND.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "nand_w25n_emul.h"

LOG_MODULE_REGISTER(nand_emul, LOG_LEVEL_INF);

/* APS51216BA Octal RAM linear burst (physical wire cmds). */
#define APS_Z8_LINEAR_BURST_WRITE 0xA0U
#define APS_Z8_LINEAR_BURST_READ  0x20U

/* Reserved PSRAM window used as the physical mirror of the soft array. */
#define NAND_EMUL_PSRAM_BASE 0x00020000U
#define XFER_TIMEOUT_MS      100U
/* Simulated tPROG / tRD while OIP is set (soft; not datasheet-accurate). */
#define NAND_EMUL_BUSY_US    50U
#define PAGE_ALIGN_4K        4096U

#define DMA_BUFF __attribute__((section(".ambiq_dma_buff"))) __aligned(PAGE_ALIGN_4K)

/* PIO cmd/status scratch — SSRAM nocache (same section Whoop bounce uses). */
static uint8_t cmd_scratch[4] DMA_BUFF;
static uint8_t status_scratch[4] DMA_BUFF;

/* Ongoing stress stats — logged from this module every 64 ops, not only at init. */
static uint32_t nand_prog_ok;
static uint32_t nand_prog_fail;
static uint32_t nand_read_ok;
static uint32_t nand_read_fail;
static uint32_t nand_cycle_ok;
static uint32_t nand_cycle_fail;

static void nand_emul_log_result(bool is_program, uint16_t page, int err)
{
	/* Per-op detail only on failure; success cadence is stress_cycle. */
	if (is_program) {
		if (err) {
			nand_prog_fail++;
			if ((nand_prog_fail & 0x3FU) == 1U) {
				LOG_ERR("program fail page=%u err=%d ok=%u fail=%u",
					page, err, nand_prog_ok, nand_prog_fail);
			}
		} else {
			nand_prog_ok++;
		}
	} else if (err) {
		nand_read_fail++;
		if ((nand_read_fail & 0x3FU) == 1U) {
			LOG_ERR("read fail page=%u err=%d ok=%u fail=%u", page, err,
				nand_read_ok, nand_read_fail);
		}
	} else {
		nand_read_ok++;
	}
}

static int whoop_claim(struct nand_emul_ctx *ctx)
{
	return mspi_dev_config(ctx->bus, ctx->dev_id, MSPI_DEVICE_CONFIG_NONE,
			       NULL);
}

static int whoop_release(struct nand_emul_ctx *ctx)
{
	return mspi_get_channel_status(ctx->bus, 0);
}

static int whoop_timing_set(struct nand_emul_ctx *ctx)
{
	if (!ctx->timing_valid || ctx->timing == NULL) {
		return 0;
	}

	return mspi_timing_config(ctx->bus, ctx->dev_id, ctx->timing_mask,
				  ctx->timing);
}

/*
 * Physical Whoop-shaped xfer: single sync packet, 100 ms, caller cache.
 * cmd_tag is the W25N opcode we are emulating (logged / soft only);
 * wire cmd is always APS linear burst.
 */
static int whoop_xfer(struct nand_emul_ctx *ctx, enum mspi_xfer_mode mode,
		      enum mspi_xfer_direction dir, uint8_t w25n_opcode,
		      uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t wire_cmd = (dir == MSPI_TX) ? APS_Z8_LINEAR_BURST_WRITE
					    : APS_Z8_LINEAR_BURST_READ;
	struct mspi_xfer_packet packet = {
		.dir = dir,
		.cmd = wire_cmd,
		.address = addr,
		.num_bytes = len,
		.data_buf = buf,
		.cb_mask = MSPI_BUS_NO_CB,
	};
	struct mspi_xfer xfer = {
		.async = false,
		.xfer_mode = mode,
		.tx_dummy = ctx->dev_cfg->tx_dummy,
		.rx_dummy = ctx->dev_cfg->rx_dummy,
		.cmd_length = ctx->dev_cfg->cmd_length,
		.addr_length = ctx->dev_cfg->addr_length,
		.hold_ce = false,
		.priority = MSPI_XFER_PRIORITY_MEDIUM,
		.packets = &packet,
		.num_packet = 1,
		.timeout = XFER_TIMEOUT_MS,
	};

	ARG_UNUSED(w25n_opcode);

	if (dir == MSPI_TX) {
		sys_cache_data_flush_range(buf, len);
	} else {
		sys_cache_data_flush_and_invd_range(buf, len);
	}

	return mspi_transceive(ctx->bus, ctx->dev_id, &xfer);
}

static uint32_t page_psram_addr(uint16_t page)
{
	return NAND_EMUL_PSRAM_BASE + ((uint32_t)page * NAND_EMUL_PAGE_SIZE);
}

int nand_emul_init(struct nand_emul_ctx *ctx, const struct device *bus,
		   struct mspi_dev_id *dev_id, const struct mspi_dev_cfg *dev_cfg,
		   struct mspi_ambiq_timing_cfg *timing, uint32_t timing_mask,
		   bool timing_valid, uint8_t *page_cache, uint8_t *array)
{
	if (ctx == NULL || bus == NULL || dev_id == NULL || dev_cfg == NULL ||
	    page_cache == NULL || array == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->bus = bus;
	ctx->dev_id = dev_id;
	ctx->dev_cfg = dev_cfg;
	ctx->timing = timing;
	ctx->timing_mask = timing_mask;
	ctx->timing_valid = timing_valid;
	ctx->page_cache = page_cache;
	ctx->array = array;
	ctx->status = 0;
	ctx->cache_page = 0;
	ctx->cache_valid = false;

	memset(page_cache, 0xFF, NAND_EMUL_PAGE_SIZE);
	memset(array, 0xFF, NAND_EMUL_ARRAY_SIZE);

	nand_prog_ok = 0;
	nand_prog_fail = 0;
	nand_read_ok = 0;
	nand_read_fail = 0;
	nand_cycle_ok = 0;
	nand_cycle_fail = 0;

	LOG_INF("W25N emul ready: %u pages x %u B, PSRAM mirror @ 0x%08x "
		"(MSPI stress = this emulator)",
		NAND_EMUL_PAGES, NAND_EMUL_PAGE_SIZE, NAND_EMUL_PSRAM_BASE);

	return 0;
}

int nand_emul_page_program(struct nand_emul_ctx *ctx, uint16_t page,
			   uint16_t column, const uint8_t *data, size_t len)
{
	int ret;
	int final = 0;

	if (ctx == NULL || data == NULL) {
		return -EINVAL;
	}
	if (page >= NAND_EMUL_PAGES || column >= NAND_EMUL_PAGE_SIZE ||
	    (column + len) > NAND_EMUL_PAGE_SIZE || len == 0U) {
		return -EINVAL;
	}

	ret = whoop_claim(ctx);
	if (ret) {
		LOG_ERR("claim failed (err %d) page=%u", ret, page);
		nand_emul_log_result(true, page, ret);
		return ret;
	}

	ret = whoop_timing_set(ctx);
	if (ret) {
		final = ret;
		goto out;
	}

	/* 1) WREN — soft WEL; PIO short write (Whoop dumb / width != 4). */
	ctx->status |= W25N_STATUS_WEL;
	cmd_scratch[0] = W25N_CMD_WREN;
	cmd_scratch[1] = 0;
	cmd_scratch[2] = 0;
	cmd_scratch[3] = 0;
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_TX, W25N_CMD_WREN,
			 page_psram_addr(page), cmd_scratch, sizeof(cmd_scratch));
	if (ret) {
		LOG_ERR("WREN PIO failed (err %d)", ret);
		final = ret;
		goto out;
	}

	/* Soft PROGRAM_LOAD into page cache. */
	memcpy(ctx->page_cache + column, data, len);
	if (column != 0U || len != NAND_EMUL_PAGE_SIZE) {
		/* Partial load: rest of cache stays as-is (W25N behavior). */
	}
	ctx->cache_page = page;
	ctx->cache_valid = true;

	/* 2) PROGRAM_LOAD — DMA page data (Whoop width == 4). */
	ret = whoop_xfer(ctx, MSPI_DMA, MSPI_TX, W25N_CMD_PROGRAM_LOAD,
			 page_psram_addr(page), ctx->page_cache,
			 NAND_EMUL_PAGE_SIZE);
	if (ret) {
		LOG_ERR("PROGRAM_LOAD DMA failed (err %d)", ret);
		final = ret;
		goto out;
	}

	/* Soft PROGRAM_EXECUTE: cache → array, pulse OIP. */
	ctx->status |= W25N_STATUS_OIP;
	memcpy(ctx->array + ((size_t)page * NAND_EMUL_PAGE_SIZE), ctx->page_cache,
	       NAND_EMUL_PAGE_SIZE);
	k_busy_wait(NAND_EMUL_BUSY_US);
	ctx->status &= (uint8_t)~(W25N_STATUS_OIP | W25N_STATUS_WEL);

	/* 3) PROGRAM_EXECUTE — PIO marker write under same claim. */
	cmd_scratch[0] = W25N_CMD_PROGRAM_EXECUTE;
	cmd_scratch[1] = (uint8_t)(page >> 8);
	cmd_scratch[2] = (uint8_t)page;
	cmd_scratch[3] = 0;
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_TX, W25N_CMD_PROGRAM_EXECUTE,
			 page_psram_addr(page), cmd_scratch, sizeof(cmd_scratch));
	if (ret) {
		LOG_ERR("PROGRAM_EXECUTE PIO failed (err %d)", ret);
		final = ret;
		goto out;
	}

	/* 4) GET_FEATURE status — PIO read (Whoop status poll shape). */
	status_scratch[0] = ctx->status;
	status_scratch[1] = W25N_FEAT_STATUS;
	status_scratch[2] = 0;
	status_scratch[3] = 0;
	/* Seed status byte into PSRAM so RX has something to fetch. */
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_TX, W25N_CMD_GET_FEATURE,
			 page_psram_addr(page), status_scratch,
			 sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	memset(status_scratch, 0, sizeof(status_scratch));
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_RX, W25N_CMD_GET_FEATURE,
			 page_psram_addr(page), status_scratch,
			 sizeof(status_scratch));
	if (ret) {
		LOG_ERR("GET_FEATURE PIO failed (err %d)", ret);
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(status_scratch, sizeof(status_scratch));

	if ((ctx->status & W25N_STATUS_OIP) != 0U) {
		final = -EBUSY;
	}

out:
	ret = whoop_release(ctx);
	if (ret) {
		LOG_ERR("release failed (err %d) page=%u", ret, page);
		if (final == 0) {
			final = ret;
		}
	}

	nand_emul_log_result(true, page, final);
	return final;
}

int nand_emul_page_read(struct nand_emul_ctx *ctx, uint16_t page,
			uint16_t column, uint8_t *data, size_t len)
{
	int ret;
	int final = 0;

	if (ctx == NULL || data == NULL) {
		return -EINVAL;
	}
	if (page >= NAND_EMUL_PAGES || column >= NAND_EMUL_PAGE_SIZE ||
	    (column + len) > NAND_EMUL_PAGE_SIZE || len == 0U) {
		return -EINVAL;
	}

	ret = whoop_claim(ctx);
	if (ret) {
		LOG_ERR("claim failed (err %d) page=%u", ret, page);
		nand_emul_log_result(false, page, ret);
		return ret;
	}

	ret = whoop_timing_set(ctx);
	if (ret) {
		final = ret;
		goto out;
	}

	/* Soft PAGE_READ: array → cache, pulse OIP. */
	ctx->status |= W25N_STATUS_OIP;
	memcpy(ctx->page_cache,
	       ctx->array + ((size_t)page * NAND_EMUL_PAGE_SIZE),
	       NAND_EMUL_PAGE_SIZE);
	k_busy_wait(NAND_EMUL_BUSY_US);
	ctx->status &= (uint8_t)~W25N_STATUS_OIP;
	ctx->cache_page = page;
	ctx->cache_valid = true;

	/* 1) PAGE_READ — PIO cmd under claim. */
	cmd_scratch[0] = W25N_CMD_PAGE_READ;
	cmd_scratch[1] = (uint8_t)(page >> 8);
	cmd_scratch[2] = (uint8_t)page;
	cmd_scratch[3] = 0;
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_TX, W25N_CMD_PAGE_READ,
			 page_psram_addr(page), cmd_scratch, sizeof(cmd_scratch));
	if (ret) {
		LOG_ERR("PAGE_READ PIO failed (err %d)", ret);
		final = ret;
		goto out;
	}

	/* 2) GET_FEATURE status poll — PIO. */
	status_scratch[0] = ctx->status;
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_TX, W25N_CMD_GET_FEATURE,
			 page_psram_addr(page), status_scratch,
			 sizeof(status_scratch));
	if (ret) {
		final = ret;
		goto out;
	}
	memset(status_scratch, 0, sizeof(status_scratch));
	ret = whoop_xfer(ctx, MSPI_PIO, MSPI_RX, W25N_CMD_GET_FEATURE,
			 page_psram_addr(page), status_scratch,
			 sizeof(status_scratch));
	if (ret) {
		LOG_ERR("GET_FEATURE PIO failed (err %d)", ret);
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(status_scratch, sizeof(status_scratch));

	/* 3) READ_CACHE — DMA page out (Whoop width == 4). */
	ret = whoop_xfer(ctx, MSPI_DMA, MSPI_TX, W25N_CMD_READ_CACHE,
			 page_psram_addr(page), ctx->page_cache,
			 NAND_EMUL_PAGE_SIZE);
	if (ret) {
		LOG_ERR("READ_CACHE mirror TX failed (err %d)", ret);
		final = ret;
		goto out;
	}
	ret = whoop_xfer(ctx, MSPI_DMA, MSPI_RX, W25N_CMD_READ_CACHE,
			 page_psram_addr(page), ctx->page_cache,
			 NAND_EMUL_PAGE_SIZE);
	if (ret) {
		LOG_ERR("READ_CACHE DMA failed (err %d)", ret);
		final = ret;
		goto out;
	}
	sys_cache_data_invd_range(ctx->page_cache, NAND_EMUL_PAGE_SIZE);

	memcpy(data, ctx->page_cache + column, len);

out:
	ret = whoop_release(ctx);
	if (ret) {
		LOG_ERR("release failed (err %d) page=%u", ret, page);
		if (final == 0) {
			final = ret;
		}
	}

	nand_emul_log_result(false, page, final);
	return final;
}

int nand_emul_stress_cycle(struct nand_emul_ctx *ctx, uint32_t seq,
			   uint8_t *tx, uint8_t *rx)
{
	uint16_t page = (uint16_t)(seq % NAND_EMUL_PAGES);
	int ret;

	if (ctx == NULL || tx == NULL || rx == NULL) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < NAND_EMUL_PAGE_SIZE; i++) {
		tx[i] = (uint8_t)(seq + i);
	}
	memset(rx, 0, NAND_EMUL_PAGE_SIZE);

	ret = nand_emul_page_program(ctx, page, 0, tx, NAND_EMUL_PAGE_SIZE);
	if (ret) {
		nand_cycle_fail++;
		if ((nand_cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u page=%u err=%d (program) "
				"ok=%u fail=%u",
				seq, page, ret, nand_cycle_ok, nand_cycle_fail);
		}
		return ret;
	}

	ret = nand_emul_page_read(ctx, page, 0, rx, NAND_EMUL_PAGE_SIZE);
	if (ret) {
		nand_cycle_fail++;
		if ((nand_cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u page=%u err=%d (read) "
				"ok=%u fail=%u",
				seq, page, ret, nand_cycle_ok, nand_cycle_fail);
		}
		return ret;
	}

	if (memcmp(tx, rx, NAND_EMUL_PAGE_SIZE) != 0) {
		nand_cycle_fail++;
		if ((nand_cycle_fail & 0x3FU) == 1U) {
			LOG_ERR("stress fail seq=%u page=%u data mismatch "
				"tx0=%02x rx0=%02x ok=%u fail=%u",
				seq, page, tx[0], rx[0], nand_cycle_ok,
				nand_cycle_fail);
		}
		return -EIO;
	}

	nand_cycle_ok++;
	if ((nand_cycle_ok & 0x3FU) == 0U) {
		LOG_INF("stress ok seq=%u page=%u ok=%u fail=%u", seq, page,
			nand_cycle_ok, nand_cycle_fail);
	}

	return 0;
}

void nand_emul_stress_loop(struct nand_emul_ctx *ctx, uint8_t *tx, uint8_t *rx,
			   uint32_t loop_delay_ms)
{
	uint32_t seq = 0;

	LOG_INF("MSPI stress via NAND emul: page program/read, claim-hold, "
		"100ms xfer, always release");

	while (1) {
		(void)nand_emul_stress_cycle(ctx, seq, tx, rx);
		seq++;
		k_msleep(loop_delay_ms);
	}
}
