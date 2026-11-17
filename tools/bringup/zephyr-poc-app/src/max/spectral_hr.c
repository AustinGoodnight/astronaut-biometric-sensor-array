/* Frequency-domain heart-rate tracker. See spectral_hr.h for what it does.
 *
 * Tuned offline on four wrist recordings (green LED, sealed harness, seated, with
 * still / tapping / arm-swing / free-movement phases): 73% of seconds inside the
 * 60-80 bpm resting band overall and 78% while moving, vs 47% for a plain
 * spectral peak. */

#include "spectral_hr.h"

#include <math.h>
#include <string.h>

#define SPEC_PI        3.14159265358979f
#define SPEC_DECIM     4                         /* 100 Hz in -> 25 Hz */
#define SPEC_FS        25.0f
#define SPEC_WIN       200                       /* 8 s at 25 Hz */
#define SPEC_NFFT      1024                      /* zero-padded: 25/1024 Hz = 1.46 bpm bins */
#define SPEC_BIN_HZ    (SPEC_FS / SPEC_NFFT)
#define SPEC_K_LO      28                        /* 0.68 Hz =  41 bpm */
#define SPEC_K_HI      122                       /* 2.98 Hz = 179 bpm */
#define SPEC_NSTATE    (SPEC_K_HI - SPEC_K_LO + 1)
#define SPEC_KM_LO     21                        /* motion peak search: 0.51 Hz... */
#define SPEC_KM_HI     163                       /* ...to 3.98 Hz */
#define SPEC_AXES      3

#define SPEC_SIGMA_BPM 2.0f   /* how far heart rate may move per second (higher = follows faster, jumps more) */
#define SPEC_MOT_RMS   0.2f   /* m/s^2 in-band: below this, no motion removal */
#define SPEC_MOT_FRAC  0.2f   /* accel peaks above 20% of the strongest count as motion */
#define SPEC_MOT_PEAKS 3      /* only the strongest few */
#define SPEC_NOTCH_K   2      /* +-2 bins = +-0.05 Hz around each motion rhythm */

static const float notch_harm[] = { 0.5f, 1.0f, 2.0f, 3.0f };

/* Same 0.7-3.5 Hz Butterworth bandpass as the PPG pipeline (designed for 100 Hz). */
struct biquad { float b0, b1, b2, a1, a2, z1, z2; };
struct bandpass { struct biquad s0, s1; };
#define SPEC_BANDPASS_INIT { \
    { 0.006867866f, 0.013735731f, 0.006867866f, -1.786023501f, 0.820363944f, 0, 0 }, \
    { 1.0f, -2.0f, 1.0f, -1.948065846f, 0.950479924f, 0, 0 } }

//one filter stage. output = mix of this input, the last 2 inputs and the last 2 outputs:
//  y = b0*x + b1*x_prev + b2*x_prev2 - a1*y_prev - a2*y_prev2
//z1/z2 hold that history already partly added up, so we only keep 2 numbers instead of 4
//b's decide what frequencies get blocked, a's (feeding old outputs back in) make it resonate
static float biquad_step(struct biquad *q, float x)
{
    float y = q->b0 * x + q->z1;

    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

//2 stages back to back: s0 is a lowpass (cuts above ~3.5 Hz), s1 is a highpass (cuts below ~0.7 Hz)
//so only 0.7-3.5 Hz (42-210 bpm) makes it through
static float bandpass_step(struct bandpass *f, float x)
{
    return biquad_step(&f->s1, biquad_step(&f->s0, x));
}

static struct bandpass ppg_f = SPEC_BANDPASS_INIT;
static struct bandpass acc_f[SPEC_AXES] = { SPEC_BANDPASS_INIT, SPEC_BANDPASS_INIT, SPEC_BANDPASS_INIT };
static float ppg_dc0, acc_dc0[SPEC_AXES];
static bool primed;

static float ppg_acc, acc_acc[SPEC_AXES];        /* decimation sums */
static int dec_n;
static bool dec_has_acc;

static float ppg_ring[SPEC_WIN], acc_ring[SPEC_AXES][SPEC_WIN];
static bool acc_valid[SPEC_WIN];
static int head, count;

static float score[SPEC_NSTATE];
static bool have_score;

/* Scratch for the FFTs (static: too big for a work queue stack). */
static float fre[SPEC_NFFT], fim[SPEC_NFFT];
static float hann[SPEC_WIN];
static bool hann_ready;
static float pspec[SPEC_KM_HI + 1], aspec[SPEC_KM_HI + 1];

void spectral_hr_reset(void)
{
    static const struct bandpass init = SPEC_BANDPASS_INIT;

    ppg_f = init;
    for (int a = 0; a < SPEC_AXES; a++) {
        acc_f[a] = init;
    }
    primed = false;
    ppg_acc = 0;
    memset(acc_acc, 0, sizeof(acc_acc));
    dec_n = 0;
    dec_has_acc = true;
    head = count = 0;
    have_score = false;
}


//takes one 100 Hz sample (hr_main.c reads the sensor and hands it in), filters it,
//averages every 4 into one 25 Hz sample and puts that in the ring buffer
void spectral_hr_add_sample(float ppg_raw, const float *acc)
{
    //the filter starts out "thinking" the input has always been 0. raw ppg is ~100000, so the
    //first sample looks like a huge jump and the filter wobbles from it for seconds.
    //subtracting the first sample makes the input start near 0 so there's no jump
    if (!primed) {
        ppg_dc0 = ppg_raw;
        for (int a = 0; a < SPEC_AXES; a++) {
            acc_dc0[a] = acc ? acc[a] : 0;
        }
        primed = true;
    }

    //bandpass
    //filter ppg and accel the same way so motion shows up at the same bins in both ffts
    ppg_acc += bandpass_step(&ppg_f, ppg_raw - ppg_dc0);
    if (acc) {
        for (int a = 0; a < SPEC_AXES; a++) {
            acc_acc[a] += bandpass_step(&acc_f[a], acc[a] - acc_dc0[a]);
        }
    } else {
        dec_has_acc = false;
    }

    //average every 4 samples -> 25 Hz. nothing above 3.5 Hz is left so 100 Hz is overkill,
    //and the fft gets 4x cheaper. averaging is also a small extra lowpass
    if (++dec_n < SPEC_DECIM) {
        return;
    }
    //head = next slot to write = oldest sample. count stops at 200 once the 8 s window is full
    ppg_ring[head] = ppg_acc / SPEC_DECIM;
    for (int a = 0; a < SPEC_AXES; a++) {
        acc_ring[a][head] = acc_acc[a] / SPEC_DECIM;
    }
    acc_valid[head] = dec_has_acc;
    head = (head + 1) % SPEC_WIN;
    if (count < SPEC_WIN) {
        count++;
    }
    ppg_acc = 0;
    memset(acc_acc, 0, sizeof(acc_acc));
    dec_n = 0;
    dec_has_acc = true;
}



//fft transform (standard textbook version, don't need to touch it)
//first loop shuffles samples into bit-reversed order, second loop does the butterfly passes
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * SPEC_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);

        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;

            for (int j = 0; j < len / 2; j++) {
                int a = i + j, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;

                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* Power spectrum (bins 0..SPEC_KM_HI) of a ring buffer, oldest sample first, Hann-windowed. */
static void power_spectrum(const float *ring, float *out)
{
    float mean = 0;

    for (int i = 0; i < SPEC_WIN; i++) {
        mean += ring[i];
    }
    mean /= SPEC_WIN;
    //unroll the ring oldest first, remove the mean, and fade the ends to 0 (hann) so the
    //clip's start/end don't smear power across every bin
    for (int i = 0; i < SPEC_WIN; i++) {
        fre[i] = (ring[(head + i) % SPEC_WIN] - mean) * hann[i];
    }
    //zero pad 200 -> 1024. doesn't add info, just draws the same spectrum with finer bins
    memset(&fre[SPEC_WIN], 0, (SPEC_NFFT - SPEC_WIN) * sizeof(float));
    memset(fim, 0, sizeof(fim));
    fft(fre, fim, SPEC_NFFT);
    //power = re^2 + im^2. bin k is k * 25/1024 Hz
    for (int k = 0; k <= SPEC_KM_HI; k++) {
        out[k] = fre[k] * fre[k] + fim[k] * fim[k];
    }
}

bool spectral_hr_update(float *bpm, float *conf, float *motion)
{
    if (count < SPEC_WIN) {
        return false;
    }
    if (!hann_ready) {
        for (int i = 0; i < SPEC_WIN; i++) {
            hann[i] = 0.5f - 0.5f * cosf(2.0f * SPEC_PI * i / (SPEC_WIN - 1));
        }
        hann_ready = true;
    }

    power_spectrum(ppg_ring, pspec);

    //how much is the wrist moving? rms of the filtered accel over all 3 axes.
    //only trust it if every sample in the window had an accel reading
    bool acc_ok = true;
    float mot = 0;

    for (int i = 0; i < SPEC_WIN; i++) {
        acc_ok &= acc_valid[i];
        for (int a = 0; a < SPEC_AXES; a++) {
            mot += acc_ring[a][i] * acc_ring[a][i];
        }
    }
    mot = acc_ok ? sqrtf(mot / SPEC_WIN) : 0;

    //only remove motion if there's real motion. when still the accel fft is just noise and
    //its "peaks" would cut random holes in a good ppg spectrum
    if (acc_ok && mot > SPEC_MOT_RMS) {
        static float tmp[SPEC_KM_HI + 1];

        //add the 3 axis spectra together, we only care how often the arm moves, not which way
        memset(aspec, 0, sizeof(aspec));
        for (int a = 0; a < SPEC_AXES; a++) {
            power_spectrum(acc_ring[a], tmp);
            for (int k = 0; k <= SPEC_KM_HI; k++) {
                aspec[k] += tmp[k];
            }
        }
        float amax = 0;

        for (int k = SPEC_KM_LO; k <= SPEC_KM_HI; k++) {
            amax = fmaxf(amax, aspec[k]);
        }
        /* Strongest SPEC_MOT_PEAKS local maxima above the threshold. */
        int peaks[SPEC_MOT_PEAKS], np = 0;

        for (int n = 0; n < SPEC_MOT_PEAKS; n++) {
            int best = -1;

            for (int k = SPEC_KM_LO + 1; k < SPEC_KM_HI; k++) {
                bool taken = false;

                for (int m = 0; m < np; m++) {
                    taken |= peaks[m] == k;
                }
                if (!taken && aspec[k] > SPEC_MOT_FRAC * amax &&
                    aspec[k] >= aspec[k - 1] && aspec[k] >= aspec[k + 1] &&
                    (best < 0 || aspec[k] > aspec[best])) {
                    best = k;
                }
            }
            if (best < 0) {
                break;
            }
            peaks[np++] = best;
        }
        //zero those freqs out of the ppg fft, plus 0.5x/2x/3x of each:
        //  2x/3x: motion isn't a clean sine, a swing jolts at both ends
        //  0.5x: accel often peaks at step rate while the ppg picks up stride rate (half)
        //note +-2 bins (+-3 bpm) is narrower than a peak (~+-15 bpm), so the edges survive
        for (int m = 0; m < np; m++) {
            for (unsigned h = 0; h < sizeof(notch_harm) / sizeof(notch_harm[0]); h++) {
                int kc = (int)lroundf(peaks[m] * notch_harm[h]);

                for (int k = kc - SPEC_NOTCH_K; k <= kc + SPEC_NOTCH_K; k++) {
                    if (k >= 0 && k <= SPEC_KM_HI) {
                        pspec[k] = 0;
                    }
                }
            }
        }
    }

    //pick the heart rate. candidates are bins 28-122 (41-179 bpm).
    //total = all the power in that range, so each bin can be scored as a share of it
    float total = 0;

    for (int k = SPEC_K_LO; k <= SPEC_K_HI; k++) {
        total += pspec[k];
    }
    total += 1e-12f;

    /* Causal Viterbi step: best path ending at each candidate, penalising fast changes. */
    static float next[SPEC_NSTATE];
    const float bin_bpm = SPEC_BIN_HZ * 60.0f;
    float best = -INFINITY;
    int besti = 0;

    //for each candidate bpm i:
    //  le   = how good bpm i looks this second, log(its share of power)
    //         30% -> -1.2, 1% -> -4.6, notched (0) -> -13.8 which basically rules it out
    //  prev = best score from last second, minus a cost for jumping from there to i
    //         cost = 0.5*(jump/2bpm)^2 -> 1.5 bpm costs 0.27, 10 bpm costs 12.8, 30 bpm costs 105
    //  score[i] = le + prev = running total, so it remembers the whole history in 95 numbers
    //so a 1 second motion spike can't pull it off, but a slow real hr change gets followed
    for (int i = 0; i < SPEC_NSTATE; i++) {
        float le = logf(pspec[SPEC_K_LO + i] / total + 1e-6f);
        float prev = 0;

        //first update has no history yet, so it's just the biggest peak
        if (have_score) {
            prev = -INFINITY;
            for (int j = 0; j < SPEC_NSTATE; j++) {
                float d = (float)(i - j) * bin_bpm / SPEC_SIGMA_BPM;
                prev = fmaxf(prev, score[j] - 0.5f * d * d);
            }
        }
        next[i] = le + prev;
        if (next[i] > best) {
            best = next[i];
            besti = i;
        }
    }
    for (int i = 0; i < SPEC_NSTATE; i++) {
        score[i] = next[i] - best; /* keep numbers small */
    }
    have_score = true;

    //answer = whichever candidate has the best running score right now
    //conf = that bin's share of power. near 0 means it's coasting on history, not this second's data
    *bpm = (float)(SPEC_K_LO + besti) * bin_bpm;
    *conf = pspec[SPEC_K_LO + besti] / total;
    *motion = mot;
    return true;
}
