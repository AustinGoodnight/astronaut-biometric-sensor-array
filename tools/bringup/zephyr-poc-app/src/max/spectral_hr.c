/* Frequency-domain heart-rate tracker. See spectral_hr.h for what it does.
 *
 * Tuned offline on four wrist recordings (green LED, sealed harness, seated, with
 * still / tapping / arm-swing / free-movement phases): 73% of seconds inside the
 * 60-80 bpm resting band overall and 78% while moving, vs 47% for a plain
 * spectral peak. */

#include "spectral_hr.h"

#include <math.h>
#include <string.h>

#include <arm_math.h>   /* CMSIS-DSP (ARM's DSP library, part of NCS): the FFT */

#include "bandpass.h"

#define SPEC_PI               3.14159265358979f
#define SPEC_DOWNSAMPLE       4                  /* average every 4 samples: 100 Hz in -> 25 Hz */
#define SPEC_FS               25.0f              /* sample rate after downsampling */
#define SPEC_WINDOW_LEN       200                /* 8 s at 25 Hz */
#define SPEC_FFT_LEN          1024               /* window zero-padded to this: 25/1024 Hz = 1.46 bpm bins.
                                                  * must stay 1024: see arm_rfft_fast_init_1024_f32 below */
#define SPEC_BIN_HZ           (SPEC_FS / SPEC_FFT_LEN)
#define SPEC_HR_BIN_MIN       28                 /* heart-rate candidates: 0.68 Hz =  41 bpm... */
#define SPEC_HR_BIN_MAX       122                /* ...to 2.98 Hz = 179 bpm */
#define SPEC_HR_BINS          (SPEC_HR_BIN_MAX - SPEC_HR_BIN_MIN + 1) /* 95 candidates */
#define SPEC_MOTION_BIN_MIN   21                 /* motion peak search: 0.51 Hz... */
#define SPEC_MOTION_BIN_MAX   163                /* ...to 3.98 Hz */
#define SPEC_AXES             3

#define SPEC_HR_CHANGE_BPM    2.0f   /* how far heart rate may move per second (higher = follows faster, jumps more) */
#define SPEC_MOTION_MIN_RMS   0.2f   /* m/s^2 in-band: below this, no motion removal */
#define SPEC_MOTION_PEAK_FRAC 0.2f   /* accel peaks above 20% of the strongest count as motion */
#define SPEC_MOTION_MAX_PEAKS 3      /* only the strongest few */
/* +-2 bins = +-0.05 Hz (+-3 bpm) around each motion rhythm. That only takes the top off a
 * motion peak (an 8 s Hann window smears it over ~+-7 bpm at half height), but +-5 was
 * tried on a 9-minute wrist recording (2026-11-16) and did worse: wider notches cut away
 * the real pulse peak whenever it sits near a motion harmonic, and the tracker then locks
 * onto a different track (15-30 bpm off) for tens of seconds. */
#define SPEC_NOTCH_HALF_WIDTH 2

/* Each motion rhythm is notched at these multiples of its frequency. */
static const float notch_multiples[] = { 0.5f, 1.0f, 2.0f, 3.0f };

/* Same 0.7-3.5 Hz bandpass as the classic path (bandpass.h), on PPG and accel alike.
 * Set up by spectral_hr_reset(). */
static struct bandpass ppg_filter;
static struct bandpass acc_filter[SPEC_AXES];
static float ppg_first, acc_first[SPEC_AXES];    /* the first sample, subtracted from all later ones */
static bool first_sample = true; /* true until the first sample since reset has been handled */

/* Downsampling: 4 filtered samples are summed here, then averaged into one 25 Hz sample. */
static float ppg_sum, acc_sum[SPEC_AXES];
static int sum_count;            /* how many are in the sums (0..3) */
static bool sum_has_acc;         /* false if any of them had no accel reading */

/* The last 8 s at 25 Hz, as ring buffers. next_slot is where the next sample goes, which
 * (once the window is full) is also where the oldest one is. */
static float ppg_window[SPEC_WINDOW_LEN], acc_window[SPEC_AXES][SPEC_WINDOW_LEN];
static bool acc_present[SPEC_WINDOW_LEN];        /* that 25 Hz sample had accel data */
static int next_slot, window_filled;             /* window_filled stops at 200 */

/* The tracker's running score for each of the 95 candidate bins (0 = the leader). */
static float track_score[SPEC_HR_BINS];
static bool first_update = true; /* true until the tracker has a score history */

/* The FFT (CMSIS-DSP's real FFT): its setup, and its input/output buffers (static: too
 * big for a work queue stack). */
static arm_rfft_fast_instance_f32 fft_setup;
static float fft_in[SPEC_FFT_LEN], fft_out[SPEC_FFT_LEN];
static float hann_window[SPEC_WINDOW_LEN];
static bool setup_done;          /* fft_setup and hann_window are built on first use */
static float ppg_power[SPEC_MOTION_BIN_MAX + 1], acc_power[SPEC_MOTION_BIN_MAX + 1];

void spectral_hr_reset(void)
{
    bandpass_reset(&ppg_filter);
    for (int a = 0; a < SPEC_AXES; a++) {
        bandpass_reset(&acc_filter[a]);
    }
    first_sample = true;
    ppg_sum = 0;
    memset(acc_sum, 0, sizeof(acc_sum));
    sum_count = 0;
    sum_has_acc = true;
    next_slot = window_filled = 0;
    first_update = true;
}


//takes one 100 Hz sample (hr_main.c reads the sensor and hands it in), filters it,
//averages every 4 into one 25 Hz sample and puts that in the ring buffer
void spectral_hr_add_sample(float ppg_raw, const float *acc)
{
    //the filter starts out "thinking" the input has always been 0. raw ppg is ~100000, so the
    //first sample looks like a huge jump and the filter wobbles from it for seconds.
    //subtracting the first sample makes the input start near 0 so there's no jump
    if (first_sample) {
        ppg_first = ppg_raw;
        for (int a = 0; a < SPEC_AXES; a++) {
            acc_first[a] = acc ? acc[a] : 0;
        }
        first_sample = false;
    }

    //bandpass
    //filter ppg and accel the same way so motion shows up at the same bins in both ffts
    ppg_sum += bandpass_step(&ppg_filter, ppg_raw - ppg_first);
    if (acc) {
        for (int a = 0; a < SPEC_AXES; a++) {
            acc_sum[a] += bandpass_step(&acc_filter[a], acc[a] - acc_first[a]);
        }
    } else {
        sum_has_acc = false;
    }

    //wait for 4 samples to average
    if (++sum_count < SPEC_DOWNSAMPLE) {
        return;
    }

    //add to PPG ring buffer
    ppg_window[next_slot] = ppg_sum / SPEC_DOWNSAMPLE;
    //add to acc ring buffer
    for (int a = 0; a < SPEC_AXES; a++) {
        acc_window[a][next_slot] = acc_sum[a] / SPEC_DOWNSAMPLE;
    }
    acc_present[next_slot] = sum_has_acc;
    next_slot = (next_slot + 1) % SPEC_WINDOW_LEN;
    if (window_filled < SPEC_WINDOW_LEN) {
        window_filled++;
    }
    //reset values
    ppg_sum = 0;
    memset(acc_sum, 0, sizeof(acc_sum));
    sum_count = 0;
    sum_has_acc = true;
}



//strength of fft rythm
/* Power spectrum of the last 8 s in a window: power[k] = how much of the signal is a
 * rhythm at bin k's frequency (k * 25/1024 Hz = k * 1.46 bpm), for k = 0..MOTION_BIN_MAX.
 *
 * The fft gives two numbers per bin: how well the signal matches a cosine at that
 * frequency (re) and a sine (im). Which of the two a pulse shows up in depends only on
 * where in the 8 s the beats happen to fall, so re^2 + im^2 (the power) is what's left
 * once that timing is thrown away: a big number = a strong rhythm at that frequency,
 * whenever its beats land. Only relative sizes matter; the tracker uses each bin's
 * share of the total. */
static void power_spectrum(const float *window, float *power)
{
    float mean = 0;

    //the average level isn't a rhythm, it would just be a giant bin 0 that leaks into the
    //low bins. take it out first
    for (int i = 0; i < SPEC_WINDOW_LEN; i++) {
        mean += window[i];
    }
    mean /= SPEC_WINDOW_LEN;
    //unroll the ring oldest first (next_slot = oldest), remove the mean, and fade the ends
    //to 0 (hann) so the clip's sudden start/end don't smear power across every bin
    for (int i = 0; i < SPEC_WINDOW_LEN; i++) {
        fft_in[i] = (window[(next_slot + i) % SPEC_WINDOW_LEN] - mean) * hann_window[i];
    }
    //zero pad 200 -> 1024. doesn't add info, just draws the same spectrum with finer bins
    memset(&fft_in[SPEC_WINDOW_LEN], 0, (SPEC_FFT_LEN - SPEC_WINDOW_LEN) * sizeof(float));

    //the fft (library call). last argument 0 = forward (signal -> frequencies).
    //note it uses fft_in as scratch space, so fft_in is garbage afterwards
    arm_rfft_fast_f32(&fft_setup, fft_in, fft_out, 0);

    //fft_out holds bin k's cosine part (re) and sine part (im) side by side:
    //  fft_out[2k] = re of bin k,  fft_out[2k+1] = im of bin k     (k = 1..511)
    //except the first pair: bin 0 has no sine part (always 0), so the library stores the
    //top bin (512) in its slot instead: fft_out[0] = re of bin 0, fft_out[1] = re of bin 512
    power[0] = fft_out[0] * fft_out[0];
    //power = re^2 + im^2. bin k is k * 25/1024 Hz. only bins up to MOTION_BIN_MAX (239 bpm)
    //are ever looked at
    for (int k = 1; k <= SPEC_MOTION_BIN_MAX; k++) {
        float re = fft_out[2 * k], im = fft_out[2 * k + 1];

        power[k] = re * re + im * im;
    }
}

//how much is the wrist moving? rms of the filtered accel over all 3 axes, in m/s^2.
//only trusted if every sample in the window had an accel reading (0 otherwise, which
//turns motion removal off)
static float motion_level(void)
{
    bool acc_complete = true;
    float motion_rms = 0;

    for (int i = 0; i < SPEC_WINDOW_LEN; i++) {
        acc_complete &= acc_present[i];
        for (int a = 0; a < SPEC_AXES; a++) {
            motion_rms += acc_window[a][i] * acc_window[a][i];
        }
    }
    return acc_complete ? sqrtf(motion_rms / SPEC_WINDOW_LEN) : 0;
}

//find the arm's rhythms in the accel spectrum and zero them (and their 0.5x/2x/3x) in
//ppg_power, so the tracker can't pick motion as the heart rate
static void notch_motion(void)
{
    static float axis_power[SPEC_MOTION_BIN_MAX + 1];

    //add the 3 axis spectra together, we only care how often the arm moves, not which way
    memset(acc_power, 0, sizeof(acc_power));
    for (int a = 0; a < SPEC_AXES; a++) {
        power_spectrum(acc_window[a], axis_power);
        for (int k = 0; k <= SPEC_MOTION_BIN_MAX; k++) {
            acc_power[k] += axis_power[k];
        }
    }
    float acc_max = 0;

    for (int k = SPEC_MOTION_BIN_MIN; k <= SPEC_MOTION_BIN_MAX; k++) {
        acc_max = fmaxf(acc_max, acc_power[k]);
    }
    //find up to 3 motion peaks, strongest first. each pass picks the highest bin that is
    //a local peak (>= both neighbours), above 20% of the strongest, and not picked yet
    int motion_peaks[SPEC_MOTION_MAX_PEAKS], n_peaks = 0;

    for (int pass = 0; pass < SPEC_MOTION_MAX_PEAKS; pass++) {
        int best_bin = -1;

        for (int k = SPEC_MOTION_BIN_MIN + 1; k < SPEC_MOTION_BIN_MAX; k++) {
            bool already_picked = false;

            for (int p = 0; p < n_peaks; p++) {
                already_picked |= motion_peaks[p] == k;
            }
            if (!already_picked && acc_power[k] > SPEC_MOTION_PEAK_FRAC * acc_max &&
                acc_power[k] >= acc_power[k - 1] && acc_power[k] >= acc_power[k + 1] &&
                (best_bin < 0 || acc_power[k] > acc_power[best_bin])) {
                best_bin = k;
            }
        }
        if (best_bin < 0) {                  //no more peaks big enough
            break;
        }
        motion_peaks[n_peaks++] = best_bin;
    }
    //zero those freqs out of the ppg fft, plus 0.5x/2x/3x of each:
    //  2x/3x: motion isn't a clean sine, a swing jolts at both ends
    //  0.5x: accel often peaks at step rate while the ppg picks up stride rate (half)
    //+-NOTCH_HALF_WIDTH bins (+-3 bpm) only removes the top of each motion peak; its edges
    //survive, but notching wider costs more real pulse than it saves (see SPEC_NOTCH_HALF_WIDTH)
    for (int p = 0; p < n_peaks; p++) {
        for (unsigned m = 0; m < sizeof(notch_multiples) / sizeof(notch_multiples[0]); m++) {
            int center = (int)lroundf(motion_peaks[p] * notch_multiples[m]);

            for (int k = center - SPEC_NOTCH_HALF_WIDTH; k <= center + SPEC_NOTCH_HALF_WIDTH; k++) {
                if (k >= 0 && k <= SPEC_MOTION_BIN_MAX) {
                    ppg_power[k] = 0;
                }
            }
        }
    }
}

/* The tracker. One causal Viterbi step: best path ending at each candidate, penalising
 * fast changes. Updates track_score and returns the winning candidate (0..94, add
 * SPEC_HR_BIN_MIN for the bin).
 *
 * for each candidate bpm i:
 *   evidence     = how good bpm i looks this second, log(its share of power)
 *                  30% -> -1.2, 1% -> -4.6, notched (0) -> -13.8 which basically rules it out
 *   best_arrival = best score from last second, minus a cost for jumping from there to i
 *                  cost = 0.5*(jump/2bpm)^2 -> 1.5 bpm costs 0.27, 10 bpm costs 12.8, 30 bpm costs 105
 *   new_score[i] = evidence + best_arrival = running total, so it remembers the whole
 *                  history in 95 numbers
 * so a 1 second motion spike can't pull it off, but a slow real hr change gets followed */
static int track_step(float band_power)
{
    static float new_score[SPEC_HR_BINS];
    const float bin_bpm = SPEC_BIN_HZ * 60.0f;
    float best_score = -INFINITY;
    int winner = 0;

    for (int i = 0; i < SPEC_HR_BINS; i++) {
        float evidence = logf(ppg_power[SPEC_HR_BIN_MIN + i] / band_power + 1e-6f);
        float best_arrival = 0;

        //first update has no history yet, so it's just the biggest peak
        if (!first_update) {
            best_arrival = -INFINITY;
            for (int from = 0; from < SPEC_HR_BINS; from++) {
                //jump from bin `from` to bin i, in units of HR_CHANGE_BPM
                float jump = (float)(i - from) * bin_bpm / SPEC_HR_CHANGE_BPM;

                best_arrival = fmaxf(best_arrival, track_score[from] - 0.5f * jump * jump);
            }
        }
        new_score[i] = evidence + best_arrival;
        if (new_score[i] > best_score) {
            best_score = new_score[i];
            winner = i;
        }
    }
    for (int i = 0; i < SPEC_HR_BINS; i++) {
        track_score[i] = new_score[i] - best_score; /* keep numbers small: leader = 0 */
    }
    first_update = false;
    return winner;
}

//once a second: spectrum -> remove motion -> tracker -> answer
bool spectral_hr_update(float *bpm, float *conf, float *motion)
{
    if (window_filled < SPEC_WINDOW_LEN) {
        return false;
    }
    //one-time setup, the first time it's needed
    if (!setup_done) {
        //the fft library precomputes its tables for this size. the _1024 version only
        //pulls in the 1024-point tables (the general one links every size up to 4096,
        //a lot of flash for nothing)
        arm_rfft_fast_init_1024_f32(&fft_setup);
        //hann = 0 at both ends, 1 in the middle
        for (int i = 0; i < SPEC_WINDOW_LEN; i++) {
            hann_window[i] = 0.5f - 0.5f * cosf(2.0f * SPEC_PI * i / (SPEC_WINDOW_LEN - 1));
        }
        setup_done = true;
    }

    //1. how tall each frequency bar is in the last 8 s of ppg
    power_spectrum(ppg_window, ppg_power);

    //2. remove motion, but only if there's real motion. when still the accel fft is just
    //noise and its "peaks" would cut random holes in a good ppg spectrum
    float motion_rms = motion_level();

    if (motion_rms > SPEC_MOTION_MIN_RMS) {
        notch_motion();
    }

    //3. pick the heart rate. candidates are bins 28-122 (41-179 bpm).
    //band_power = all the power in that range, so each bin can be scored as a share of it
    float band_power = 0;

    for (int k = SPEC_HR_BIN_MIN; k <= SPEC_HR_BIN_MAX; k++) {
        band_power += ppg_power[k];
    }
    band_power += 1e-12f;                    //never divide by 0
    int winner = track_step(band_power);

    //4. answer = whichever candidate has the best running score right now
    //conf = that bin's share of power. near 0 means it's coasting on history, not this second's data
    *bpm = (float)(SPEC_HR_BIN_MIN + winner) * SPEC_BIN_HZ * 60.0f;
    *conf = ppg_power[SPEC_HR_BIN_MIN + winner] / band_power;
    *motion = motion_rms;
    return true;
}
