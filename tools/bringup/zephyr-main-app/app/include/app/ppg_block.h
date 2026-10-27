/*
 * One PPG FIFO read plus the accel samples drained in the same pass.
 * Allocated by acq from a memory slab, passed by pointer to proc, freed by
 * proc (ARCHITECTURE.md §3.1-3.2).
 */

#ifndef APP_PPG_BLOCK_H_
#define APP_PPG_BLOCK_H_

#include <stdint.h>
#include <zephyr/sys/util.h>

/** MAX30101 FIFO depth. */
#define PPG_BLOCK_MAX_SAMPLES   32
/** Green, plus IR for skin-contact detection. */
#define PPG_MAX_CHANNELS        2
#define ACCEL_BLOCK_MAX_SAMPLES 32

/** The chip FIFO overflowed before this read (samples were lost). */
#define PPG_BLK_PPG_OVF   BIT(0)
#define PPG_BLK_ACCEL_OVF BIT(1)

/*
 * NOTE: PPG and accel run on separate chip oscillators that drift by a few
 * percent. Sample i's time is reconstructed backwards from t_read_ms at the
 * nominal rate. Processing must RESAMPLE accel onto the PPG timebase (using
 * the actual sample counts per block to estimate the true rate ratio)
 * before using it for motion-artifact reduction. Never assume the two index
 * sequences line up.
 */
struct ppg_block {
	/** k_uptime (ms) when the FIFO read completed. */
	uint32_t t_read_ms;
	/** Nominal rates as configured on the chips. */
	uint16_t ppg_rate_hz;
	uint16_t accel_rate_hz;
	/** Samples per channel in this block (0 = accel-only block). */
	uint8_t ppg_n;
	/** Active LED channels. */
	uint8_t ppg_channels;
	/** May be 0 if the accel is FAILED or not present. */
	uint8_t accel_n;
	/** PPG_BLK_* flags. */
	uint8_t flags;
	/** 18-bit raw ADC counts. */
	uint32_t ppg[PPG_BLOCK_MAX_SAMPLES][PPG_MAX_CHANNELS];
	/** x, y, z in mg. */
	int16_t accel[ACCEL_BLOCK_MAX_SAMPLES][3];
};

/**
 * @brief Return a block to the slab.
 *
 * Called by proc once every processing module is done with the block.
 * Never blocks.
 */
void ppg_block_free(struct ppg_block *blk);

#endif /* APP_PPG_BLOCK_H_ */
