#ifndef SPECTRAL_HR_H
#define SPECTRAL_HR_H

/* Frequency-domain heart-rate tracker ("tracker D" from the 2026-11-14 bench tests).
 *
 * Every second: take the last 8 s of PPG, find which rhythms are in it (FFT), delete
 * the rhythms the accelerometer says are motion (and their 0.5x/2x/3x), then pick the
 * heart-rate peak that is most consistent with the recent past (causal Viterbi: heart
 * rate can only move ~2 bpm per second). No Zephyr dependencies (only CMSIS-DSP, which
 * builds anywhere), so it also builds and runs on a host against bench recordings. */

#include <stdbool.h>

/* Forget everything (new session, LED change). Must also be called once before the
 * first sample: it sets up the filters. */
void spectral_hr_reset(void);

/* Feed one raw PPG sample at 100 Hz. acc: accelerometer x/y/z in m/s^2 for the same
 * instant, or NULL if there is none (no IMU): then no motion removal is done. */
void spectral_hr_add_sample(float ppg_raw, const float *acc);

/* Call once per second. Returns false while the first 8 s are still filling. On true,
 * *bpm is the tracked heart rate, *conf the share of the heart-rate band's power in the
 * chosen peak (0..1), *motion the in-band wrist acceleration (m/s^2 rms). */
bool spectral_hr_update(float *bpm, float *conf, float *motion);

#endif /* SPECTRAL_HR_H */
