#ifndef BANDPASS_H
#define BANDPASS_H

/* 2nd-order Butterworth bandpass, 0.7-3.5 Hz (42-210 bpm) at 100 Hz: the one filter
 * every heart-rate path uses, for the PPG and the accelerometer alike, so motion shows
 * up the same way in both. Coefficients from
 * scipy.signal.butter(2, [0.7, 3.5], 'bandpass', fs=100, output='sos'). They assume
 * fs = 100; a few % of sample-rate error just shifts the band edges slightly. */

struct biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

//2 stages back to back: s0 is a lowpass (cuts above ~3.5 Hz), s1 is a highpass (cuts below ~0.7 Hz)
//so only 0.7-3.5 Hz (42-210 bpm) makes it through
struct bandpass {
    struct biquad s0, s1;
};

#define BANDPASS_INIT {                                                                \
    .s0 = { 0.006867866f, 0.013735731f, 0.006867866f, -1.786023501f, 0.820363944f },   \
    .s1 = { 1.0f, -2.0f, 1.0f, -1.948065846f, 0.950479924f },                          \
}

//one filter stage. output = mix of this input, the last 2 inputs and the last 2 outputs:
//  y = b0*x + b1*x_prev + b2*x_prev2 - a1*y_prev - a2*y_prev2
//z1/z2 hold that history already partly added up, so we only keep 2 numbers instead of 4
//b's decide what frequencies get blocked, a's (feeding old outputs back in) make it resonate
//(transposed direct form II)
static inline float biquad_step(struct biquad *q, float x)
{
    float y = q->b0 * x + q->z1;

    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static inline float bandpass_step(struct bandpass *f, float x)
{
    return biquad_step(&f->s1, biquad_step(&f->s0, x));
}

/* Forget the history (as if the input had always been 0); keeps the coefficients. */
static inline void bandpass_reset(struct bandpass *f)
{
    f->s0.z1 = f->s0.z2 = 0;
    f->s1.z1 = f->s1.z2 = 0;
}

#endif /* BANDPASS_H */
