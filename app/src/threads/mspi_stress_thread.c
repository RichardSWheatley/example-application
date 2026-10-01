/*
 * MSPI stress thread — boot timing scan, then hand off to NAND emulator stress.
 *
 * The workload under test is soft W25N page ops (nand_emul_stress_loop).
 * MSPI hardware is exercised only through that emulator.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mspi_ambiq.h"
#include "nand_w25n_emul.h"

#include "../shared/shared.h"

LOG_MODULE_REGISTER(mspi_stress, LOG_LEVEL_INF);

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(aps51216ba), okay)
#error "mspi_stress_thread requires aps51216ba status okay"
#endif

#ifndef CONFIG_MSPI_AMBIQ_TIMING_SCAN
#error "Enable CONFIG_MSPI_AMBIQ_TIMING_SCAN for Whoop boot timing scan"
#endif

#define APS_NODE      DT_NODELABEL(aps51216ba)
#define MSPI_BUS_NODE DT_BUS(APS_NODE)

#define FREQ_SCAN_START_HZ  24000000U
#define FREQ_SCAN_TARGET_HZ 96000000U

#define LOOP_DELAY_MS 10U
#define PAGE_ALIGN_4K 4096U

#define MSPI_AMBIQ_TIMING_CONFIG_DT(n)                                                            \
	{                                                                                         \
		.ui8WriteLatency = DT_PROP_BY_IDX(n, ambiq_timing_config, 0),                     \
		.ui8TurnAround   = DT_PROP_BY_IDX(n, ambiq_timing_config, 1),                     \
		.bTxNeg          = DT_PROP_BY_IDX(n, ambiq_timing_config, 2),                     \
		.bRxNeg          = DT_PROP_BY_IDX(n, ambiq_timing_config, 3),                     \
		.bRxCap          = DT_PROP_BY_IDX(n, ambiq_timing_config, 4),                     \
		.ui32TxDQSDelay  = DT_PROP_BY_IDX(n, ambiq_timing_config, 5),                     \
		.ui32RxDQSDelay  = DT_PROP_BY_IDX(n, ambiq_timing_config, 6),                     \
	}

#define MSPI_AMBIQ_TIMING_CONFIG_MASK_DT(n) DT_PROP(n, ambiq_timing_config_mask)

#define DMA_BUFF __attribute__((section(".ambiq_dma_buff"))) __aligned(PAGE_ALIGN_4K)

static struct mspi_dev_id mspi_id = MSPI_DEVICE_ID_DT(APS_NODE);
static const struct mspi_dev_cfg mspi_cfg = MSPI_DEVICE_CONFIG_DT(APS_NODE);
static const struct mspi_xip_cfg xip_cfg_dt = MSPI_XIP_CONFIG_DT(APS_NODE);
static const struct mspi_ambiq_timing_cfg timing_cfg_dt =
	MSPI_AMBIQ_TIMING_CONFIG_DT(APS_NODE);
static const uint32_t timing_cfg_mask = MSPI_AMBIQ_TIMING_CONFIG_MASK_DT(APS_NODE);

static struct mspi_ambiq_timing_cfg saved_timing;
static bool timing_scan_valid;

static uint8_t page_cache[NAND_EMUL_PAGE_SIZE] DMA_BUFF;
static uint8_t nand_array[NAND_EMUL_ARRAY_SIZE];
static uint8_t host_tx[NAND_EMUL_PAGE_SIZE] DMA_BUFF;
static uint8_t host_rx[NAND_EMUL_PAGE_SIZE] DMA_BUFF;

static struct nand_emul_ctx nand_ctx;

BUILD_ASSERT(NAND_EMUL_PAGE_SIZE <= PAGE_ALIGN_4K,
	     "page cache must fit in one 4 KB page");
BUILD_ASSERT((NAND_EMUL_PAGE_SIZE % 4U) == 0U, "page size must be word-aligned");

static int whoop_claim(const struct device *bus)
{
	return mspi_dev_config(bus, &mspi_id, MSPI_DEVICE_CONFIG_NONE, NULL);
}

static int whoop_release(const struct device *bus)
{
	return mspi_get_channel_status(bus, 0);
}

static int whoop_timing_set(const struct device *bus,
			    struct mspi_ambiq_timing_cfg *timing)
{
	int ret = mspi_timing_config(bus, &mspi_id, timing_cfg_mask, timing);

	if (ret) {
		LOG_ERR("mspi_timing_config SET failed (err %d)", ret);
	}

	return ret;
}

static int set_freq(const struct device *bus, uint32_t freq_hz)
{
	struct mspi_dev_cfg cfg = mspi_cfg;
	int ret;

	cfg.freq = freq_hz;
	ret = mspi_dev_config(bus, &mspi_id, MSPI_DEVICE_CONFIG_FREQUENCY, &cfg);
	if (ret) {
		LOG_ERR("set freq %u failed (err %d)", freq_hz, ret);
	} else {
		LOG_INF("MSPI frequency set to %u Hz", freq_hz);
	}

	return ret;
}

static int xip_set(const struct device *bus, bool enable)
{
	struct mspi_xip_cfg xip = xip_cfg_dt;
	int ret;

	xip.enable = enable;
	ret = mspi_xip_config(bus, &mspi_id, &xip);
	if (ret) {
		LOG_ERR("mspi_xip_config(enable=%d) failed (err %d)", enable, ret);
	}

	return ret;
}

static bool buf_crosses_4k(const void *buf, size_t len)
{
	uintptr_t start = (uintptr_t)buf;
	uintptr_t end = start + len - 1U;

	return (start & ~(uintptr_t)(PAGE_ALIGN_4K - 1U)) !=
	       (end & ~(uintptr_t)(PAGE_ALIGN_4K - 1U));
}

static int boot_timing_scan(const struct device *bus, const struct device *psram)
{
	struct mspi_ambiq_timing_cfg baseline = timing_cfg_dt;
	struct mspi_ambiq_timing_scan scan;
	uint32_t xip_base;
	int ret;

	ret = whoop_claim(bus);
	if (ret) {
		LOG_ERR("boot scan claim failed (err %d)", ret);
		return ret;
	}

	ret = xip_set(bus, true);
	if (ret) {
		goto out;
	}

	ret = set_freq(bus, FREQ_SCAN_START_HZ);
	if (ret) {
		goto out;
	}

	ret = whoop_timing_set(bus, &baseline);
	if (ret) {
		goto out;
	}

	ret = set_freq(bus, FREQ_SCAN_TARGET_HZ);
	if (ret) {
		goto out;
	}

	memset(&scan, 0, sizeof(scan));
	scan.range.txdqs_end = 10;
	scan.range.rxdqs_end = 31;
	scan.scan_type = MSPI_AMBIQ_TIMING_SCAN_MEMC;
	scan.min_window = 6;
	xip_base = DT_REG_ADDR_BY_IDX(MSPI_BUS_NODE, 1);
	scan.device_addr = xip_base + xip_cfg_dt.address_offset + (xip_cfg_dt.size / 2U);

	LOG_INF("Boot timing scan @ %u Hz (from %u)", FREQ_SCAN_TARGET_HZ,
		FREQ_SCAN_START_HZ);

	ret = mspi_ambiq_timing_scan(psram, bus, &mspi_id, timing_cfg_mask, &baseline,
				     &scan);
	if (ret) {
		LOG_ERR("mspi_ambiq_timing_scan failed (err %d)", ret);
		goto out;
	}

	saved_timing = scan.result;
	timing_scan_valid = true;

	LOG_INF("Timing stored: WL=%u TA=%u TxNeg=%d RxNeg=%d RxCap=%d "
		"TxDQS=%u RxDQS=%u",
		saved_timing.ui8WriteLatency, saved_timing.ui8TurnAround,
		saved_timing.bTxNeg, saved_timing.bRxNeg, saved_timing.bRxCap,
		saved_timing.ui32TxDQSDelay, saved_timing.ui32RxDQSDelay);

	ret = whoop_timing_set(bus, &saved_timing);
	if (ret) {
		timing_scan_valid = false;
		goto out;
	}

	ret = xip_set(bus, false);

out:
	(void)whoop_release(bus);
	return ret;
}

void mspi_stress_thread(void)
{
	const struct device *bus = DEVICE_DT_GET(MSPI_BUS_NODE);
	const struct device *psram = DEVICE_DT_GET(APS_NODE);
	int ret;

	k_sem_take(&graphics_ready_sem, K_FOREVER);

	if (!device_is_ready(bus) || !device_is_ready(psram)) {
		LOG_ERR("MSPI bus/psram not ready");
		return;
	}

	if (buf_crosses_4k(page_cache, NAND_EMUL_PAGE_SIZE) ||
	    buf_crosses_4k(host_tx, NAND_EMUL_PAGE_SIZE) ||
	    buf_crosses_4k(host_rx, NAND_EMUL_PAGE_SIZE)) {
		LOG_ERR("DMA buffers cross a 4 KB page — abort");
		return;
	}

	if (boot_timing_scan(bus, psram)) {
		LOG_ERR("Boot timing scan failed; aborting NAND emul stress");
		return;
	}

	ret = nand_emul_init(&nand_ctx, bus, &mspi_id, &mspi_cfg, &saved_timing,
			     timing_cfg_mask, timing_scan_valid, page_cache,
			     nand_array);
	if (ret) {
		LOG_ERR("nand_emul_init failed (err %d)", ret);
		return;
	}

	LOG_INF("Handing off to nand_emul stress (MSPI only via NAND emul)");
	nand_emul_stress_loop(&nand_ctx, host_tx, host_rx, LOOP_DELAY_MS);
}
