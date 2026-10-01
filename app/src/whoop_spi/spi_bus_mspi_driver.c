#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mspi_ambiq.h"
#include "spi_bus_mspi_driver.h"

LOG_MODULE_REGISTER(spi_bus_mspi, LOG_LEVEL_INF);

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(aps51216ba), okay)
#error "spi_bus_mspi_driver requires aps51216ba status okay"
#endif

#ifndef CONFIG_MSPI_AMBIQ_TIMING_SCAN
#error "Enable CONFIG_MSPI_AMBIQ_TIMING_SCAN for Whoop boot timing scan"
#endif

#define APS_NODE      DT_NODELABEL(aps51216ba)
#define MSPI_BUS_NODE DT_BUS(APS_NODE)

#define FREQ_SCAN_START_HZ  24000000U
#define FREQ_SCAN_TARGET_HZ 96000000U
#define XFER_TIMEOUT_MS     100U

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

static const struct device *bus_dev;
static const struct device *psram_dev;
static struct mspi_dev_id mspi_id = MSPI_DEVICE_ID_DT(APS_NODE);
static struct mspi_dev_cfg mspi_cfg = MSPI_DEVICE_CONFIG_DT(APS_NODE);
static const struct mspi_xip_cfg xip_cfg_dt = MSPI_XIP_CONFIG_DT(APS_NODE);
static const struct mspi_ambiq_timing_cfg timing_cfg_dt =
	MSPI_AMBIQ_TIMING_CONFIG_DT(APS_NODE);
static const uint32_t timing_cfg_mask = MSPI_AMBIQ_TIMING_CONFIG_MASK_DT(APS_NODE);
static struct mspi_ambiq_timing_cfg saved_timing;
static bool timing_scan_valid;
static uint32_t active_freq_hz;

int spi_bus_mspi_driver_init(void)
{
	bus_dev = DEVICE_DT_GET(MSPI_BUS_NODE);
	psram_dev = DEVICE_DT_GET(APS_NODE);

	if (!device_is_ready(bus_dev) || !device_is_ready(psram_dev)) {
		return -ENODEV;
	}

	active_freq_hz = mspi_cfg.freq;
	return 0;
}

int spi_bus_mspi_driver_acquire(void)
{
	return mspi_dev_config(bus_dev, &mspi_id, MSPI_DEVICE_CONFIG_NONE, NULL);
}

int spi_bus_mspi_driver_configure_all(uint32_t freq_hz)
{
	struct mspi_dev_cfg cfg = mspi_cfg;

	if (freq_hz != 0U) {
		cfg.freq = freq_hz;
	}

	int ret = mspi_dev_config(bus_dev, &mspi_id, MSPI_DEVICE_CONFIG_ALL, &cfg);

	if (ret == 0) {
		mspi_cfg = cfg;
		active_freq_hz = cfg.freq;
	}

	return ret;
}

int spi_bus_mspi_driver_transceive(bool use_dma, bool is_tx, uint8_t cmd,
				   uint32_t addr, uint8_t *buf, uint32_t len)
{
	struct mspi_xfer_packet packet = {
		.dir = is_tx ? MSPI_TX : MSPI_RX,
		.cmd = cmd,
		.address = addr,
		.num_bytes = len,
		.data_buf = buf,
		.cb_mask = MSPI_BUS_NO_CB,
	};
	struct mspi_xfer xfer = {
		.async = false,
		.xfer_mode = use_dma ? MSPI_DMA : MSPI_PIO,
		.tx_dummy = mspi_cfg.tx_dummy,
		.rx_dummy = mspi_cfg.rx_dummy,
		.cmd_length = mspi_cfg.cmd_length,
		.addr_length = mspi_cfg.addr_length,
		.hold_ce = false,
		.priority = MSPI_XFER_PRIORITY_MEDIUM,
		.packets = &packet,
		.num_packet = 1,
		.timeout = XFER_TIMEOUT_MS,
	};

	if (is_tx) {
		sys_cache_data_flush_range(buf, len);
	} else {
		sys_cache_data_flush_and_invd_range(buf, len);
	}

	return mspi_transceive(bus_dev, &mspi_id, &xfer);
}

int spi_bus_mspi_driver_release(void)
{
	return mspi_get_channel_status(bus_dev, 0);
}

uint32_t spi_bus_mspi_driver_get_freq(void)
{
	return active_freq_hz;
}

int spi_bus_mspi_driver_boot_timing_scan(void)
{
	struct mspi_ambiq_timing_cfg baseline = timing_cfg_dt;
	struct mspi_ambiq_timing_scan scan;
	struct mspi_xip_cfg xip = xip_cfg_dt;
	struct mspi_dev_cfg cfg;
	uint32_t xip_base;
	int ret;

	ret = spi_bus_mspi_driver_acquire();
	if (ret) {
		return ret;
	}

	xip.enable = true;
	ret = mspi_xip_config(bus_dev, &mspi_id, &xip);
	if (ret) {
		goto out;
	}

	cfg = mspi_cfg;
	cfg.freq = FREQ_SCAN_START_HZ;
	ret = mspi_dev_config(bus_dev, &mspi_id, MSPI_DEVICE_CONFIG_ALL, &cfg);
	if (ret) {
		goto out;
	}
	mspi_cfg = cfg;

	ret = mspi_timing_config(bus_dev, &mspi_id, timing_cfg_mask, &baseline);
	if (ret) {
		goto out;
	}

	cfg.freq = FREQ_SCAN_TARGET_HZ;
	ret = mspi_dev_config(bus_dev, &mspi_id, MSPI_DEVICE_CONFIG_ALL, &cfg);
	if (ret) {
		goto out;
	}
	mspi_cfg = cfg;
	active_freq_hz = cfg.freq;

	memset(&scan, 0, sizeof(scan));
	scan.range.txdqs_end = 10;
	scan.range.rxdqs_end = 31;
	scan.scan_type = MSPI_AMBIQ_TIMING_SCAN_MEMC;
	scan.min_window = 6;
	xip_base = DT_REG_ADDR_BY_IDX(MSPI_BUS_NODE, 1);
	scan.device_addr = xip_base + xip_cfg_dt.address_offset + (xip_cfg_dt.size / 2U);

	LOG_INF("Boot timing scan @ %u Hz (from %u)", FREQ_SCAN_TARGET_HZ,
		FREQ_SCAN_START_HZ);

	ret = mspi_ambiq_timing_scan(psram_dev, bus_dev, &mspi_id, timing_cfg_mask,
				     &baseline, &scan);
	if (ret) {
		goto out;
	}

	saved_timing = scan.result;
	timing_scan_valid = true;

	LOG_INF("Timing stored: WL=%u TA=%u TxNeg=%d RxNeg=%d RxCap=%d "
		"TxDQS=%u RxDQS=%u",
		saved_timing.ui8WriteLatency, saved_timing.ui8TurnAround,
		saved_timing.bTxNeg, saved_timing.bRxNeg, saved_timing.bRxCap,
		saved_timing.ui32TxDQSDelay, saved_timing.ui32RxDQSDelay);

	ret = mspi_timing_config(bus_dev, &mspi_id, timing_cfg_mask, &saved_timing);
	if (ret) {
		timing_scan_valid = false;
		goto out;
	}

	xip.enable = false;
	ret = mspi_xip_config(bus_dev, &mspi_id, &xip);

out:
	(void)spi_bus_mspi_driver_release();
	return ret;
}
