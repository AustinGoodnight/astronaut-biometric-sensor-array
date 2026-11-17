#ifndef HR_MAIN_H
#define HR_MAIN_H

/* Heart rate from the MAX30101: reads the sensor, runs both heart-rate algorithms
 * (spectral_hr.h, classic_hr.h) and publishes one of them. */

#include <stdbool.h>
#include <stdint.h>

/* The FIFO fills at 100 samples/s and holds 32 (320 ms). Each poll drains it, so this
 * only needs to be comfortably shorter than 320 ms. */
#define PPG_POLL_PERIOD_MS 50

/* Set up the sensor and start a session. 0 on success, negative errno. */
int ppg_init(void);

/* Drain the sensor FIFO and run every sample through the pipeline. Call every
 * PPG_POLL_PERIOD_MS. */
void ppg_poll(void);

/* The published heart rate. Returns false (and leaves *bpm alone) if there is none. */
bool hr_latest(int32_t *bpm);

#endif /* HR_MAIN_H */
