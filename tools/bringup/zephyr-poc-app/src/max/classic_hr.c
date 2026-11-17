/* Classic (autocorrelation) heart rate. See classic_hr.h for the six steps.
 * Mirrored in JS by makeClassic() in tools/hr-algo-compare/index.html. */

#include "classic_hr.h"

#include <math.h>
#include <string.h>

#include "bandpass.h"

#define FS_NOMINAL      100.0f   /* 400 sps / 4x on-chip averaging */
#define BASELINE_RATE   0.01f    /* baseline moves 1% of the way per sample: ~1 s to catch up */
#define PULSE_SIZE_RATE 0.02f    /* pulse-size average moves 2% per sample (for acdc) */
#define STEP_FRAC       0.03f    /* sample-to-sample jump > 3% of DC = artifact (real beats ~0.5%) */
#define STEP_CLEAR_FRAC 0.10f    /* > 10% is a finger placed/removed: drop the reading too */
#define PAUSE_STEP_SEC  1.5f     /* pause after an artifact */
#define PAUSE_START_SEC 2.0f     /* pause at session start, while the filter settles */

#define DOWNSAMPLE      4        /* average every 4 samples: 100 Hz in -> 25 Hz window */
#define WINDOW_LEN      200      /* 8 s @ 25 Hz */
#define MAX_LAG         40       /* covers 42 bpm (lag ~36 @ 25 Hz) with margin */
#define BPM_MIN         42.0f
#define BPM_MAX         220.0f
#define MIN_CORRELATION 0.50f    /* windows less periodic than this are rejected */
#define PEAK_FRAC       0.85f    /* the first peak at least this close to the best one wins */
#define MEDIAN_OF       5        /* the reading is the median of the last 5 estimates */

//all of classic's memory in one struct, so a reset is a single memset and nothing
//can be forgotten
static struct {
    float fs;                    /* sample rate, ~100 Hz (see classic_hr_set_fs) */
    unsigned sampleCount;        /* samples since reset; all timing is counted in samples */
    bool firstSample;            /* true until the first sample since reset has been handled */
    float baseline;              /* slow DC level, subtracted before the bandpass */
    float lastRaw;               /* previous raw sample, to measure the jump to this one */
    float pulseSize;             /* slow average of |filtered| ~ pulse amplitude */
    struct bandpass filter;      /* the 0.7-3.5 Hz filter, with its own history */
    unsigned pauseUntil;         /* sampleCount when the current pause ends */

    float window[WINDOW_LEN];    /* the last 8 s at 25 Hz, as a ring buffer */
    int windowNext;              /* next slot to write (= the oldest, once full) */
    int windowFilled;            /* how many slots hold data (max 200) */
    float sum;                   /* sum of the 100 Hz samples for the next 25 Hz one... */
    int sumCount;                /* ...and how many are in it (0..3) */

    float lastCorrelation;       /* best autocorrelation of the last estimate (status r=) */
    float recent[MEDIAN_OF];     /* last estimates, for the median (a ring of 5) */
    int recentNext;              /* next slot to write in recent */
    int recentCount;             /* how many are filled (max 5) */
    bool hasReading;             /* there is a reading to show */
    float reading;               /* the reading: median of recent */
    unsigned lastGoodSample;     /* sampleCount of the last good window */
} state;

void classic_hr_reset(void)
{
    static const struct bandpass init = BANDPASS_INIT;

    //zero everything, then set the things that mustn't start at 0: the filter's
    //coefficients, the sample rate, and "the next sample is the first one"
    memset(&state, 0, sizeof(state));
    state.filter = init;
    state.fs = FS_NOMINAL;
    state.firstSample = true;
}

void classic_hr_set_fs(float fs)
{
    state.fs = fs;
}

//forget the reading and the median history (but keep the 8 s window if there is one)
static void clear_reading(void)
{
    state.hasReading = false;
    state.recentCount = 0;
    state.recentNext = 0;
}

/* Restart the filter and window after an artifact (finger moved/removed, etc.) so its
 * transient can't poison the autocorrelation window. */
static void pause_after_step(float raw)
{
    //the filter would ring for seconds after a big jump, so wipe its history
    bandpass_reset(&state.filter);
    //restart the baseline at the new level instead of letting it crawl there (~1 s)
    state.baseline = raw;
    //throw away the window: it holds the jump. it has to fill for 8 s again
    state.windowNext = state.windowFilled = 0;
    state.sum = 0;
    state.sumCount = 0;
    //and ignore the next 1.5 s (150 samples) while the filter settles
    state.pauseUntil = state.sampleCount + (unsigned)(PAUSE_STEP_SEC * state.fs);
}

//called for every 100 Hz sample. does steps 1-4: DC, bandpass, step check, window
bool classic_hr_add_sample(float raw, float *filtered)
{
    bool artifact = false;

    state.sampleCount++;
    //1. DC: start the baseline on the first sample so the bandpass doesn't see a giant step
    //(raw is ~70000; starting the baseline at 0 would look like a 70000-count jump)
    if (state.firstSample) {
        state.firstSample = false;
        state.baseline = raw;
        state.lastRaw = raw;
        //at the very start, wait 2 s (200 samples) before trusting anything
        state.pauseUntil = (unsigned)(PAUSE_START_SEC * state.fs);
    }

    //3. step artifact. a heartbeat moves raw by ~0.5% of DC between samples; a finger
    //moving, the LED changing or the sensor lifting moves it by much more in one go
    float step = fabsf(raw - state.lastRaw);

    if (step > STEP_FRAC * state.baseline) {          //> 3% of DC: e.g. > 2100 counts at 70000
        //only worth logging if we weren't already paused (one log per artifact, not per sample)
        artifact = state.sampleCount >= state.pauseUntil;
        if (step > STEP_CLEAR_FRAC * state.baseline) { //> 10%: finger placed/removed, old reading is stale
            clear_reading();
        }
        pause_after_step(raw);
    }
    state.lastRaw = raw;

    //1 + 2. remove DC, bandpass
    //in = how far this sample is from the slow baseline (the pulse, plus noise)
    float in = raw - state.baseline;

    //move the baseline 1% of the way toward this sample. over ~100 samples (1 s) it
    //catches up with slow drift, but it's far too slow to follow a heartbeat
    state.baseline += BASELINE_RATE * in;
    //keep only 0.7-3.5 Hz (42-210 bpm): removes leftover drift, breathing and fast noise
    float f = bandpass_step(&state.filter, in);

    *filtered = f;                                    //hr_main sends this to the testing page
    if (state.sampleCount < state.pauseUntil) {       //paused: the filter is still settling, so
        return artifact;                              //don't let this sample into the window
    }

    //pulse size for the status line's acdc= (and the spectral "is there a pulse" check):
    //a slow average of |f|, moving 2% of the way each sample
    state.pulseSize += PULSE_SIZE_RATE * (fabsf(f) - state.pulseSize);

    //4. the 8 s window: average every 4 samples -> one 25 Hz sample. nothing above
    //3.5 Hz is left after the bandpass, so 25 Hz loses nothing and the math below is 16x
    //cheaper (4x fewer samples, 4x fewer lags)
    state.sum += f;
    if (++state.sumCount >= DOWNSAMPLE) {
        //write it at windowNext and move on; after 200 writes windowNext wraps to 0 and
        //starts overwriting the oldest, so window always holds the newest 200 = the last 8 s
        state.window[state.windowNext] = state.sum / DOWNSAMPLE;
        state.windowNext = (state.windowNext + 1) % WINDOW_LEN;
        if (state.windowFilled < WINDOW_LEN) {
            state.windowFilled++;
        }
        state.sum = 0;
        state.sumCount = 0;
    }
    return artifact;
}

/* 5. Autocorrelation estimate over the window. A periodic pulse lines up with itself
 * shifted by one beat period. Lags are searched only over the human range, and the
 * *first* strong peak is taken rather than the best one, since 2 beats (half the bpm)
 * line up almost as well. The peak is refined with a parabola through it and its
 * neighbours (1 lag ~ 4 bpm at 75 bpm, so this matters). */
static bool estimate(float *bpmOut, float *correlationOut)
{
    static float centered[WINDOW_LEN];       //the window, oldest first, mean removed (static: off the stack)
    static float corr[MAX_LAG + 2];          //corr[lag] = match score at that shift
    int n = state.windowFilled;
    int oldest = (state.windowNext - n + WINDOW_LEN) % WINDOW_LEN;
    float mean = 0;

    //copy the ring out in time order (oldest first), adding it up for the mean
    for (int i = 0; i < n; i++) {
        centered[i] = state.window[(oldest + i) % WINDOW_LEN];
        mean += centered[i];
    }
    mean /= n;
    float power = 0;

    //center it on 0, and measure its power (= its match with itself at shift 0).
    //every other shift is divided by power, so a perfect match scores 1.0 whatever the
    //pulse size
    for (int i = 0; i < n; i++) {
        centered[i] -= mean;
        power += centered[i] * centered[i];
    }
    power /= n;
    if (power <= 0) {                        //flat window: nothing to find
        return false;
    }

    //which shifts can be a heartbeat? at 25 Hz, a shift of L samples = L/25 s per beat =
    //1500/L bpm. so 220 bpm -> L = 6.8 -> 6, and 42 bpm -> L = 35.7 -> 36. only those
    //are searched, so e.g. breathing (shift ~100) can never be picked
    float windowFs = state.fs / DOWNSAMPLE;
    int lagMin = (int)(windowFs * 60.0f / BPM_MAX);
    int lagMax = (int)(windowFs * 60.0f / BPM_MIN + 0.5f);

    if (lagMin < 2) {
        lagMin = 2;
    }
    if (lagMax > MAX_LAG - 1) {
        lagMax = MAX_LAG - 1;
    }
    //corr[lag] = how well the window matches itself shifted by lag, 1.0 = perfectly.
    //multiply each sample by the one lag samples later and add up: where peaks land on
    //peaks and dips on dips every product is positive and the sum is big; shifted half
    //a beat, peaks land on dips and it goes negative. one extra lag each side (lagMin-1,
    //lagMax+1) so the peak test below can look at both neighbours
    for (int lag = lagMin - 1; lag <= lagMax + 1; lag++) {
        float sum = 0;

        for (int i = 0; i + lag < n; i++) {
            sum += centered[i] * centered[i + lag];
        }
        //divide by the number of products (fewer at bigger shifts) and by power
        corr[lag] = (sum / (n - lag)) / power;
    }

    //best match anywhere in the heartbeat range
    float bestCorr = 0;

    for (int lag = lagMin; lag <= lagMax; lag++) {
        bestCorr = fmaxf(bestCorr, corr[lag]);
    }
    *correlationOut = bestCorr;
    if (bestCorr < MIN_CORRELATION) {        //nothing lines up even halfway: noise or motion
        return false;
    }

    //take the FIRST peak (shortest shift) that's within 85% of the best, not the best
    //itself. a pulse at 75 bpm (lag 20) also lines up at 2 beats (lag 40 = 37 bpm) and
    //sometimes slightly better; the shortest one is the real beat
    int beatLag = -1;

    for (int lag = lagMin; lag <= lagMax; lag++) {
        //a peak: at least as high as both neighbours, and high enough
        if (corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1] && corr[lag] >= PEAK_FRAC * bestCorr) {
            beatLag = lag;
            break;
        }
    }
    if (beatLag < 0) {                       //the best is at the edge of the range, not a peak
        return false;
    }

    //lags are whole samples: lag 20 = 75 bpm but lag 19 = 79 and lag 21 = 71, too coarse.
    //fit a parabola through the peak and its two neighbours and use the parabola's top,
    //somewhere between beatLag-1 and beatLag+1 (offset = how far from beatLag, in samples)
    float left = corr[beatLag - 1], mid = corr[beatLag], right = corr[beatLag + 1];
    float curve = left - 2.0f * mid + right;
    float offset = (curve != 0.0f) ? 0.5f * (left - right) / curve : 0.0f;

    offset = fminf(fmaxf(offset, -1.0f), 1.0f); //never move more than one sample
    //beat period = (beatLag + offset) / 25 s, so bpm = 60 / period = 1500 / (beatLag + offset)
    *bpmOut = 60.0f * windowFs / ((float)beatLag + offset);
    return true;
}

//middle value of the n (<= 5) numbers in v, without changing v
static float median(const float *v, int n)
{
    float sorted[MEDIAN_OF];

    memcpy(sorted, v, n * sizeof(float));
    //insertion sort: take each value and slide it left past every bigger one
    for (int i = 1; i < n; i++) {
        float key = sorted[i];
        int j = i - 1;

        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return sorted[n / 2];                    //sorted, so the middle one is the median
}

//called once a second by hr_main. does steps 5 and 6 and reports the result
void classic_hr_update(struct classic_hr_status *out)
{
    float bpm;

    //5. one estimate per second once the window is full
    if (state.windowFilled >= WINDOW_LEN && estimate(&bpm, &state.lastCorrelation)) {
        //6. median of the last 5, so one odd window can't move the reading:
        //e.g. 74, 75, 112, 76, 75 -> 75. recent is a ring of 5 like the window
        state.recent[state.recentNext] = bpm;
        state.recentNext = (state.recentNext + 1) % MEDIAN_OF;
        if (state.recentCount < MEDIAN_OF) {
            state.recentCount++;
        }
        state.reading = median(state.recent, state.recentCount);
        state.hasReading = true;
        state.lastGoodSample = state.sampleCount;
    }

    //6. drop the reading once there's been no good window for MAX_AGE (5 s = 500 samples):
    //covers both failed estimates and a window that keeps getting reset by artifacts
    out->cleared = false;
    if (state.hasReading &&
        (float)(state.sampleCount - state.lastGoodSample) >= CLASSIC_HR_MAX_AGE_SEC * state.fs) {
        clear_reading();
        out->cleared = true;
    }

    //fill in the report for hr_main's status line
    out->valid = state.hasReading;
    out->bpm = state.reading;
    out->warm = state.windowFilled < WINDOW_LEN;
    out->filled_s = state.windowFilled * DOWNSAMPLE / state.fs;  //200 x 4 / 100 Hz = 8 s
    out->window_s = WINDOW_LEN * DOWNSAMPLE / state.fs;
    out->r = state.lastCorrelation;
    //pulse size as a share of DC, e.g. 0.0012 = 0.12%
    out->acdc = (state.baseline > 0) ? state.pulseSize / state.baseline : 0;
}
