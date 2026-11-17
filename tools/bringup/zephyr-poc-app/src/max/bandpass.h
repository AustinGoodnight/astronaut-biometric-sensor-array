#ifndef BANDPASS_H
#define BANDPASS_H

/* 2nd-order Butterworth bandpass, 0.7-3.5 Hz (42-210 bpm) at 100 Hz: the one filter
 * every heart-rate path uses, for the PPG and the accelerometer alike, so motion shows
 * up the same way in both. The filtering itself is CMSIS-DSP's (ARM's DSP library,
 * ships with NCS, turned on in prj.conf); this file only holds the coefficients and
 * wraps the three calls.
 *
 * How to use one:
 *   struct bandpass f;
 *   bandpass_reset(&f);              before the first sample, and to forget history
 *   y = bandpass_step(&f, x);        every sample, in order, none skipped
 *
 * The filter remembers recent samples, which is how it can tell slow from fast: each
 * output is a weighted mix of the last few inputs and outputs. Each struct bandpass has
 * its own memory, so every signal (PPG, each accel axis) needs its own one. */

#include <string.h>

#include <arm_math.h>

#define BANDPASS_STAGES 2

/* Coefficients from scipy.signal.butter(2, [0.7, 3.5], 'bandpass', fs=100, output='sos'),
 * designed once on a PC and pasted in. They assume fs = 100; a few % of sample-rate error
 * just shifts the band edges slightly. Two stages, 5 numbers each: b0, b1, b2, a1, a2.
 *
 * a1 and a2 are scipy's with the SIGN FLIPPED: scipy's formula subtracts the a terms,
 * CMSIS's adds them (y = b0*x + b1*x_prev + b2*x_prev2 + a1*y_prev + a2*y_prev2). Getting
 * this wrong makes the filter blow up. */
static const float32_t bandpass_coeffs[5 * BANDPASS_STAGES] = {
    /* stage 1, lowpass: cuts above ~3.5 Hz */
    0.006867866f, 0.013735731f, 0.006867866f, 1.786023501f, -0.820363944f,
    /* stage 2, highpass: cuts below ~0.7 Hz */
    1.0f, -2.0f, 1.0f, 1.948065846f, -0.950479924f,
};

struct bandpass {
    arm_biquad_cascade_df2T_instance_f32 cmsis;   /* the library's handle */
    float32_t memory[2 * BANDPASS_STAGES];        /* the filter's history, 2 numbers per stage */
};

/* Set up the filter and forget its history (as if the input had always been 0). */
static inline void bandpass_reset(struct bandpass *f)
{
    //points the library at our coefficients and memory, and zeroes the memory
    arm_biquad_cascade_df2T_init_f32(&f->cmsis, BANDPASS_STAGES, bandpass_coeffs, f->memory);
}

/* Filter one sample: returns it with everything outside 0.7-3.5 Hz removed. */
static inline float bandpass_step(struct bandpass *f, float x)
{
    float y;

    //the library filters a block of samples at a time; ours arrive one by one, so a block of 1
    arm_biquad_cascade_df2T_f32(&f->cmsis, &x, &y, 1);
    return y;
}

#endif /* BANDPASS_H */
