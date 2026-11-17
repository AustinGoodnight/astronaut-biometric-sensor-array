#ifndef CLASSIC_HR_H
#define CLASSIC_HR_H

/* Time-domain heart rate (the "classic" path). Works with the wrist still; under motion
 * use the spectral tracker (spectral_hr.h), which removes motion and this doesn't.
 *
 *   1. subtract the slow DC baseline
 *   2. bandpass 0.7-3.5 Hz (42-210 bpm)
 *   3. a jump of > 3% of DC in one sample (finger moved, LED changed) restarts the
 *      filter and window and pauses 1.5 s
 *   4. keep the last 8 s at 25 Hz
 *   5. every second, find the beat period by autocorrelation: the first lag where the
 *      window lines up with itself (>= 85% of the best match, which must be >= 0.5);
 *      bpm = 60 * 25 / lag
 *   6. report the median of the last 5 estimates; "--" after 5 s without a good one
 *
 * No Zephyr dependencies (only CMSIS-DSP, which builds anywhere), so it also builds and runs on a host against recordings. */

#include <stdbool.h>

/* A reading with no good window for this long is dropped (step 6). */
#define CLASSIC_HR_MAX_AGE_SEC 5

/* Forget everything (new session, LED change). Also resets the sample rate to 100 Hz.
 * Must also be called once before the first sample: it sets up the filter. */
void classic_hr_reset(void);

/* The real sample rate, if it's been measured (~100 Hz). Used for every "seconds"
 * constant and for converting the autocorrelation lag to bpm. */
void classic_hr_set_fs(float fs);

/* Feed one raw PPG sample at 100 Hz. *filtered receives the bandpassed sample (step 2).
 * Returns true if this sample was a step artifact outside a pause (worth logging). */
bool classic_hr_add_sample(float ppg_raw, float *filtered);

struct classic_hr_status {
    bool valid;        /* bpm is a reading */
    float bpm;
    bool warm;         /* the 8 s window is still filling... */
    float filled_s;    /* ...this much of it so far */
    float window_s;
    float r;           /* autocorrelation peak of the last window (0..1, want > ~0.5) */
    float acdc;        /* pulse amplitude / DC (mean |AC| / DC) */
    bool cleared;      /* this update dropped the reading (no good window for MAX_AGE) */
};

/* Call once per second (every fs samples). */
void classic_hr_update(struct classic_hr_status *st);

#endif /* CLASSIC_HR_H */
