/*
 * Audio capture thread using the Ambiq PDM (DMIC) interface.
 *
 * Based on the Zephyr DMIC sample (samples/drivers/audio/dmic).
 * Configures a single PDM channel for 16 kHz mono capture and
 * continuously reads audio blocks, logging peak amplitude statistics.
 */

#include <zephyr/kernel.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/logging/log.h>
#include "../shared/shared.h"
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

#define SAMPLE_RATE      16000U
#define SAMPLE_BIT_WIDTH 24U
#define BYTES_PER_SAMPLE (SAMPLE_BIT_WIDTH / 8)
#define NUM_CHANNELS     1U
#define READ_TIMEOUT_MS  1000

#define AUDIO_LOG_INTERVAL_MS 5000U

#define ALIGN4(x) (((x) + 3) & ~3U)
/* Block size for 100 ms of audio data, rounded up to 4 bytes for alignment. */
#define BLOCK_SIZE ALIGN4(BYTES_PER_SAMPLE * (SAMPLE_RATE / 10) * NUM_CHANNELS)
#define BLOCK_COUNT 4
K_MEM_SLAB_DEFINE_STATIC(dmic_mem_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

void audio_thread(void)
{
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(pdm0));
	int ret;
	uint32_t sample_count = 0;
	int32_t peak_level = 0;
	uint32_t last_log_ms = 0;

	/* Keep non-graphics threads blocked until LVGL startup warmup completes. */
	k_sem_take(&graphics_ready_sem, K_FOREVER);

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("DMIC device not ready, aborting audio thread");
		return;
	}

	struct pcm_stream_cfg stream = {
		.pcm_width = SAMPLE_BIT_WIDTH,
		.pcm_rate = SAMPLE_RATE,
		.block_size = BLOCK_SIZE,
		.mem_slab = &dmic_mem_slab,
	};

	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = NUM_CHANNELS,
			.req_chan_map_lo =
				dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
		},
	};

	/* Use HAL getters to deterministically ensure board clock info is available
	 * (so driver-level PLL generation can work). Prefer the existing board
	 * config if present; only set board-info here as a last-resort deterministic
	 * action. Avoid trial-and-error PLL programming from the application layer.
	 */
	{
		am_hal_clkmgr_board_info_t sClkInfo = {0};
		uint32_t rc = am_hal_clkmgr_board_info_get(&sClkInfo);

		if (rc == AM_HAL_STATUS_SUCCESS && sClkInfo.sXtalHs.ui32XtalHsFreq != 0) {
			LOG_DBG("HAL DIAG: board-info present: XTAL_HS=%u XTAL_LS=%u",
				    sClkInfo.sXtalHs.ui32XtalHsFreq, sClkInfo.sXtalLs.ui32XtalLsFreq);
		} else {
			/* Populate known board XTAL settings so CLKMGR can generate PLLs
			 * when requested by the driver. This is deterministic and uses the
			 * HAL board-info API rather than guessing display internal state.
			 */
			sClkInfo.sXtalHs.eXtalHsMode = AM_HAL_CLKMGR_XTAL_HS_MODE_XTAL;
			sClkInfo.sXtalHs.ui32XtalHsFreq = 48000000U;
			sClkInfo.sXtalLs.eXtalLsMode = AM_HAL_CLKMGR_XTAL_LS_MODE_XTAL;
			sClkInfo.sXtalLs.ui32XtalLsFreq = 32768U;
			sClkInfo.ui32ExtRefClkFreq = 0U;
			am_hal_clkmgr_board_info_set(&sClkInfo);
			LOG_DBG("HAL DIAG: board-info set: XTAL_HS=%u XTAL_LS=%u",
				    sClkInfo.sXtalHs.ui32XtalHsFreq, sClkInfo.sXtalLs.ui32XtalLsFreq);
		}

		/* Minimal diagnostics: report whether SYSPLL is already configured. */
		uint32_t syspll_freq = 0;
		uint32_t users = 0;
		rc = am_hal_clkmgr_clock_config_get(AM_HAL_CLKMGR_CLK_ID_SYSPLL, &syspll_freq, NULL);
		(void)am_hal_clkmgr_clock_status_get(AM_HAL_CLKMGR_CLK_ID_SYSPLL, &users);
		LOG_DBG("HAL DIAG: SYSPLL cfg_freq=%u user_count=%u (cfg_get rc=%u)",
			    syspll_freq, users, rc);
	}

	ret = dmic_configure(dmic_dev, &cfg);
	if (ret < 0) {
		LOG_ERR("DMIC configure failed (err %d)", ret);
		return;
	}

	ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("DMIC start failed (err %d)", ret);
		return;
	}

	LOG_INF("Audio thread started: DMIC configured, %u Hz %u-bit mono",
		SAMPLE_RATE, SAMPLE_BIT_WIDTH);

	while (1) {
		void *buffer;
		uint32_t size;

		ret = dmic_read(dmic_dev, 0, &buffer, &size, READ_TIMEOUT_MS);
		if (ret < 0) {
			LOG_WRN("DMIC read failed (err %d)", ret);
			continue;
		}

		/* Track peak amplitude across samples in this block.
		 * Support both 24-bit-in-3bytes and 24/32-bit-in-4bytes packing.
		 */
		uint8_t *bytes = buffer;
		int bytes_per_sample = BYTES_PER_SAMPLE;

		/* Heuristic: prefer 4-byte samples if buffer size divides evenly. */
		if ((size % 4) == 0) {
			bytes_per_sample = 4;
		} else if ((size % 3) == 0) {
			bytes_per_sample = 3;
		}

		size_t num_samples = size / bytes_per_sample;

		/* Detect left-aligned 24-bit packed into 32-bit words (LSB == 0). */
		bool left_aligned = false;
		if (bytes_per_sample == 4 && num_samples > 0) {
			size_t check = num_samples < 8 ? num_samples : 8;
			int left_count = 0;
			for (size_t j = 0; j < check; j++) {
				size_t off = j * 4;
				int32_t v = (int32_t)(bytes[off] | (bytes[off + 1] << 8) |
									   (bytes[off + 2] << 16) | (bytes[off + 3] << 24));
				if ((v & 0xFF) == 0) {
					left_count++;
				}
			}
			if (left_count >= (int)(check / 2)) {
				left_aligned = true;
			}
		}

		static bool diag_shown = false;
		if (!diag_shown) {
			LOG_DBG("Audio DIAG: bytes_per_sample=%d num_samples=%u left_aligned=%d",
				    bytes_per_sample, (unsigned)num_samples, left_aligned);
			diag_shown = true;
		}

		if (bytes_per_sample == 4) {
			/* Interpret each sample as a 32-bit word (little-endian).
			 * Left-justified 24-bit samples are in bits[31:8]. Right-shift
			 * and sign-extend the 24-bit value into a 32-bit signed int.
			 */
			uint32_t *words = (uint32_t *)bytes;
			for (size_t i = 0; i < num_samples; i++) {
				uint32_t w = words[i];
				uint32_t s;
				if (left_aligned) {
					s = w >> 8;
				} else {
					s = w & 0x00FFFFFFU;
				}
				/* sign-extend 24-bit to 32-bit */
				if (s & 0x00800000U) {
					s |= 0xFF000000U;
				}
				int32_t val = (int32_t)s;
				int32_t abs_val = val < 0 ? -val : val;
				if (abs_val > peak_level) {
					peak_level = abs_val;
				}
			}
		} else if (bytes_per_sample == 3) {
			for (size_t i = 0; i < num_samples; i++) {
				size_t off = i * 3;
				int32_t val = (int32_t)(bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16));
				if (val & 0x800000) {
					val |= ~0xFFFFFF;
				}
				int32_t abs_val = val < 0 ? -val : val;
				if (abs_val > peak_level) {
					peak_level = abs_val;
				}
			}
		} else {
			/* Fallback: treat as 8-bit signed */
			for (size_t i = 0; i < num_samples; i++) {
				int32_t val = (int32_t)bytes[i * bytes_per_sample];
				int32_t abs_val = val < 0 ? -val : val;
				if (abs_val > peak_level) {
					peak_level = abs_val;
				}
			}
		}

		sample_count += num_samples;
		k_mem_slab_free(&dmic_mem_slab, buffer);

		/* Periodically log audio statistics */
		uint32_t now = k_uptime_get_32();
		if ((now - last_log_ms) >= AUDIO_LOG_INTERVAL_MS) {
			LOG_INF("Audio: %u samples captured, peak level=%d",
				sample_count, (int)peak_level);
			peak_level = 0;
			last_log_ms = now;
		}
	}
}
