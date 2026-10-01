/*
 * App consumer (Whoop "flog" layer stand-in): boots bus, runs NAND cmd stress.
 * Does not include zephyr/drivers/mspi.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "nand_cmd.h"
#include "spi_bus.h"

#include "../shared/shared.h"

LOG_MODULE_REGISTER(mspi_stress, LOG_LEVEL_INF);

#define LOOP_DELAY_MS 10U
#define PAGE_ALIGN_4K 4096U

#define DMA_BUFF __attribute__((section(".ambiq_dma_buff"))) __aligned(PAGE_ALIGN_4K)

static uint8_t page_cache[NAND_CMD_PAGE_SIZE] DMA_BUFF;
static uint8_t nand_array[NAND_CMD_ARRAY_SIZE];
static uint8_t host_tx[NAND_CMD_PAGE_SIZE] DMA_BUFF;
static uint8_t host_rx[NAND_CMD_PAGE_SIZE] DMA_BUFF;
static struct nand_cmd_ctx nand_ctx;

BUILD_ASSERT(NAND_CMD_PAGE_SIZE <= PAGE_ALIGN_4K,
	     "page cache must fit in one 4 KB page");

static bool buf_crosses_4k(const void *buf, size_t len)
{
	uintptr_t start = (uintptr_t)buf;
	uintptr_t end = start + len - 1U;

	return (start & ~(uintptr_t)(PAGE_ALIGN_4K - 1U)) !=
	       (end & ~(uintptr_t)(PAGE_ALIGN_4K - 1U));
}

void mspi_stress_thread(void)
{
	int ret;

	k_sem_take(&graphics_ready_sem, K_FOREVER);

	if (buf_crosses_4k(page_cache, NAND_CMD_PAGE_SIZE) ||
	    buf_crosses_4k(host_tx, NAND_CMD_PAGE_SIZE) ||
	    buf_crosses_4k(host_rx, NAND_CMD_PAGE_SIZE)) {
		LOG_ERR("DMA buffers cross a 4 KB page — abort");
		return;
	}

	ret = spi_bus_init();
	if (ret) {
		LOG_ERR("spi_bus_init failed (err %d)", ret);
		return;
	}

	ret = spi_bus_boot_timing_scan();
	if (ret) {
		LOG_ERR("boot timing scan failed (err %d)", ret);
		return;
	}

	ret = nand_cmd_init(&nand_ctx, page_cache, nand_array);
	if (ret) {
		LOG_ERR("nand_cmd_init failed (err %d)", ret);
		return;
	}

	LOG_INF("Whoop stack: nand_cmd -> spi_bus -> spi_bus_mspi_driver");
	nand_cmd_stress_loop(&nand_ctx, host_tx, host_rx, LOOP_DELAY_MS);
}
