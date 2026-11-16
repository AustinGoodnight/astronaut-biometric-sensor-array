#!/usr/bin/env python3
"""Step-by-step picture of the spectral heart-rate tracker (src/max/spectral_hr.c).

This is a line-for-line Python port of the firmware algorithm, run on a recording,
that draws every stage for one chosen second plus the tracker over the whole file:

  1. raw PPG                          5. motion peaks + notches (if moving)
  2. bandpass + decimate to 25 Hz     6. what's left, and which bin wins
  3. Hann-windowed 8 s window         7. tracker scores over time (Viterbi)
  4. FFT power spectrum               8. bpm over time: tracker vs. plain biggest peak

Input CSV needs an `ir` column at 100 Hz (the dashboard's seq,sample_index,ir,red
format works). Optional `ax,ay,az` columns (m/s^2) are used as the accelerometer.

    python3 scripts/spectral_hr_viz.py ../hr-max30102-ble/samples.csv
    python3 scripts/spectral_hr_viz.py rec.csv --at 20 --fake-motion 1.4

--fake-motion HZ adds a SYNTHETIC arm swing (to both the PPG and a fake
accelerometer) so the motion-removal stage has something to do on recordings
without an IMU. The figure is labelled when it's used.
"""

import argparse
import csv
import math
import sys

import numpy as np
import matplotlib.pyplot as plt

# --- Same constants as spectral_hr.c ---
FS_IN = 100.0
DECIM = 4
FS = FS_IN / DECIM            # 25 Hz
WIN = 200                     # 8 s
NFFT = 1024
BIN_HZ = FS / NFFT
BIN_BPM = BIN_HZ * 60.0       # ~1.46 bpm
K_LO, K_HI = 28, 122          # 41..179 bpm: heart-rate candidates
KM_LO, KM_HI = 21, 163        # motion peak search range
SIGMA_BPM = 2.0
MOT_RMS = 0.2
MOT_FRAC = 0.2
MOT_PEAKS = 3
NOTCH_K = 2
NOTCH_HARM = (0.5, 1.0, 2.0, 3.0)

# 0.7-3.5 Hz Butterworth bandpass @ 100 Hz, two biquads (b0 b1 b2 a1 a2)
BP = ((0.006867866, 0.013735731, 0.006867866, -1.786023501, 0.820363944),
      (1.0, -2.0, 1.0, -1.948065846, 0.950479924))


def bandpass(x):
    """Same transposed direct form II as biquad_step(), primed on the first sample."""
    x = np.asarray(x, float) - x[0]
    for b0, b1, b2, a1, a2 in BP:
        y = np.empty_like(x)
        z1 = z2 = 0.0
        for i, v in enumerate(x):
            o = b0 * v + z1
            z1 = b1 * v - a1 * o + z2
            z2 = b2 * v - a2 * o
            y[i] = o
        x = y
    return x


def decimate(x):
    n = len(x) // DECIM * DECIM
    return x[:n].reshape(-1, DECIM).mean(axis=1)


HANN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(WIN) / (WIN - 1))


def power_spectrum(win):
    f = np.zeros(NFFT)
    f[:WIN] = (win - win.mean()) * HANN
    return np.abs(np.fft.fft(f)[:KM_HI + 1]) ** 2


def motion_peaks(aspec):
    amax = aspec[KM_LO:KM_HI + 1].max()
    peaks = []
    for _ in range(MOT_PEAKS):
        best = -1
        for k in range(KM_LO + 1, KM_HI):
            if (k not in peaks and aspec[k] > MOT_FRAC * amax and
                    aspec[k] >= aspec[k - 1] and aspec[k] >= aspec[k + 1] and
                    (best < 0 or aspec[k] > aspec[best])):
                best = k
        if best < 0:
            break
        peaks.append(best)
    return peaks


def run(ppg, acc):
    """Run the tracker once per second. Returns one dict per tick with everything drawn."""
    p = decimate(bandpass(ppg))
    a = None if acc is None else np.stack([decimate(bandpass(acc[:, i])) for i in range(3)])
    ticks, score = [], None
    idx = np.arange(K_HI - K_LO + 1)
    jump = 0.5 * ((idx[:, None] - idx[None, :]) * BIN_BPM / SIGMA_BPM) ** 2  # [new, old]

    for end in range(WIN, len(p) + 1, int(FS)):          # one tick per second
        w = p[end - WIN:end]
        before = power_spectrum(w)
        after = before.copy()
        mot, aspec, peaks, notched = 0.0, None, [], []
        if a is not None:
            aw = a[:, end - WIN:end]
            mot = math.sqrt((aw ** 2).sum() / WIN)
            if mot > MOT_RMS:
                aspec = sum(power_spectrum(aw[i]) for i in range(3))
                peaks = motion_peaks(aspec)
                for k0 in peaks:
                    for h in NOTCH_HARM:
                        kc = int(round(k0 * h))
                        lo, hi = max(0, kc - NOTCH_K), min(KM_HI, kc + NOTCH_K)
                        after[lo:hi + 1] = 0
                        notched.append((lo, hi, h))
        band = after[K_LO:K_HI + 1]
        share = band / (band.sum() + 1e-12)
        le = np.log(share + 1e-6)
        if score is None:
            nxt = le
        else:
            nxt = le + (score[None, :] - jump).max(axis=1)
        best = int(nxt.argmax())
        score = nxt - nxt[best]
        ticks.append(dict(t=end / FS, win=w, before=before, after=after, aspec=aspec,
                          peaks=peaks, notched=notched, mot=mot, share=share,
                          score=score.copy(), bpm=(K_LO + best) * BIN_BPM,
                          conf=share[best], naive=(K_LO + int(band.argmax())) * BIN_BPM))
    return p, ticks


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows or 'ir' not in rows[0]:
        sys.exit(f'{path}: need an "ir" column')
    ppg = np.array([float(r['ir']) for r in rows])
    acc = None
    if all(c in rows[0] for c in ('ax', 'ay', 'az')):
        # Blank cells = no IMU reading for that sample (hr-algo-compare's Save CSV): hold the last one.
        acc, last, blanks = [], [0.0, 0.0, 9.81], 0
        for r in rows:
            if all(r[c] not in ('', None) for c in ('ax', 'ay', 'az')):
                last = [float(r[c]) for c in ('ax', 'ay', 'az')]
            else:
                blanks += 1
            acc.append(last)
        acc = np.array(acc)
        if blanks == len(rows):
            acc = None
        elif blanks:
            print(f'{path}: {blanks} samples without an accelerometer reading (held the previous one)')
    return ppg, acc


def add_fake_motion(ppg, hz, amp):
    """SYNTHETIC arm swing at `hz`: added to the PPG at `amp` x the pulse size, and a
    matching fake accelerometer (m/s^2, gravity on z). Returns (ppg, acc)."""
    t = np.arange(len(ppg)) / FS_IN
    swing = np.sin(2 * np.pi * hz * t) + 0.4 * np.sin(2 * np.pi * 2 * hz * t + 0.7)
    ac_size = np.std(bandpass(ppg)[int(2 * FS_IN):int(15 * FS_IN)])  # pulse size on a quiet stretch
    acc = np.stack([3.0 * swing, 1.0 * swing, 9.81 + 0.5 * swing], axis=1)
    return ppg + amp * ac_size * swing, acc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv')
    ap.add_argument('--at', type=float, help='second to explain in detail (default: middle)')
    ap.add_argument('--fake-motion', type=float, metavar='HZ',
                    help='add a synthetic arm swing at this rate (demo of motion removal)')
    ap.add_argument('--fake-amp', type=float, default=1.0,
                    help='size of the synthetic swing in the PPG, as a multiple of the pulse (default 1)')
    ap.add_argument('-o', '--out', default='spectral_hr_viz.png')
    ap.add_argument('--show', action='store_true', help='open an interactive window too')
    args = ap.parse_args()

    ppg, acc = load(args.csv)
    fake = args.fake_motion
    if fake:
        ppg, acc = add_fake_motion(ppg, fake, args.fake_amp)

    p, ticks = run(ppg, acc)
    if not ticks:
        sys.exit('recording is shorter than the 8 s window')
    at = args.at if args.at is not None else ticks[len(ticks) // 2]['t']
    T = min(ticks, key=lambda d: abs(d['t'] - at))
    t0 = T['t'] - WIN / FS

    bins = np.arange(KM_HI + 1)
    bpm_axis = bins * BIN_BPM
    fig, ax = plt.subplots(4, 2, figsize=(15, 15))
    title = f'Spectral HR tracker on {args.csv}'
    if fake:
        title += f'   [SYNTHETIC {fake:g} Hz ({fake * 60:.0f}/min) arm swing added, {args.fake_amp:g}x pulse]'
    fig.suptitle(title, fontsize=13, weight='bold')

    # 1. raw
    t_raw = np.arange(len(ppg)) / FS_IN
    a1 = ax[0, 0]
    a1.plot(t_raw, ppg, lw=0.6, color='0.3')
    a1.axvspan(t0, T['t'], color='tab:orange', alpha=0.2, label=f'8 s window ending at {T["t"]:.0f} s')
    a1.set(title='1. Raw PPG (IR, 100 Hz): big DC level, slow drift, small pulse',
           xlabel='time (s)', ylabel='ADC counts')
    a1.legend(loc='upper right')

    # 2. filtered + decimated
    a2 = ax[0, 1]
    t_dec = np.arange(len(p)) / FS
    a2.plot(t_dec, p, lw=0.8, color='tab:blue')
    a2.axvspan(t0, T['t'], color='tab:orange', alpha=0.2)
    a2.set(title='2. Bandpass 0.7-3.5 Hz, then average every 4 samples -> 25 Hz',
           xlabel='time (s)', ylabel='filtered')

    # 3. window
    a3 = ax[1, 0]
    tw = t0 + np.arange(WIN) / FS
    w = T['win'] - T['win'].mean()
    a3.plot(tw, w, lw=0.8, color='0.6', label='window (200 samples)')
    a3.plot(tw, w * HANN, lw=1.2, color='tab:blue', label='x Hann taper')
    a3.plot(tw, HANN * np.abs(w).max(), '--', lw=0.8, color='tab:orange', label='Hann shape')
    a3.set(title='3. Last 8 s, edges tapered so the cut ends don\'t fake frequencies',
           xlabel='time (s)')
    a3.legend(loc='upper right', fontsize=8)

    # 4. spectrum before
    a4 = ax[1, 1]
    a4.bar(bpm_axis, T['before'], width=BIN_BPM, edgecolor='white', linewidth=0.4, color='tab:blue', alpha=0.35, label='FFT bins')
    a4.plot(bpm_axis, T['before'], color='tab:blue')
    a4.axvspan(K_LO * BIN_BPM, K_HI * BIN_BPM, color='tab:green', alpha=0.08,
               label='heart-rate candidates 41-179 bpm')
    a4.set(title=f'4. FFT (zero-padded to {NFFT}): power per bin, 1 bin = {BIN_BPM:.2f} bpm',
           xlabel='frequency (bpm = Hz x 60)', ylabel='power')
    a4.legend(loc='upper right', fontsize=8)

    # 5. motion
    a5 = ax[2, 0]
    if T['aspec'] is not None:
        a5.bar(bpm_axis, T['aspec'], width=BIN_BPM, edgecolor='white', linewidth=0.4, color='tab:red', alpha=0.35)
        a5.plot(bpm_axis, T['aspec'], color='tab:red', label='accelerometer spectrum (x+y+z)')
        for k in T['peaks']:
            a5.axvline(k * BIN_BPM, color='tab:red', ls=':', lw=1)
            a5.annotate(f'{k * BIN_BPM:.0f}', (k * BIN_BPM, T['aspec'][k]), color='tab:red',
                        ha='center', va='bottom')
        a5.legend(loc='upper right', fontsize=8)
        a5.set(title=f'5. Wrist moving ({T["mot"]:.2f} m/s^2 > {MOT_RMS}): '
                     f'top {len(T["peaks"])} motion peaks found',
               xlabel='frequency (bpm)', ylabel='power')
    else:
        why = 'no accelerometer in this file' if acc is None else \
            f'wrist still ({T["mot"]:.2f} m/s^2 <= {MOT_RMS})'
        a5.text(0.5, 0.5, f'5. Motion removal skipped:\n{why}\n\n(try --fake-motion 1.4)',
                ha='center', va='center', fontsize=12, transform=a5.transAxes)
        a5.set_axis_off()

    # 6. after + choice
    a6 = ax[2, 1]
    a6.bar(bpm_axis, T['before'], width=BIN_BPM, edgecolor='white', linewidth=0.4, color='0.75', alpha=0.5)
    a6.plot(bpm_axis, T['before'], color='0.75', lw=1, label='before notching')
    a6.bar(bpm_axis, T['after'], width=BIN_BPM, edgecolor='white', linewidth=0.4, color='tab:blue', alpha=0.35)
    a6.plot(bpm_axis, T['after'], color='tab:blue', lw=1.4, label='after notching')
    for lo, hi, h in T['notched']:
        a6.axvspan(lo * BIN_BPM, (hi + 1) * BIN_BPM, color='tab:red', alpha=0.15)
    a6.axvline(T['naive'], color='0.4', ls='--', label=f'biggest bin: {T["naive"]:.0f} bpm')
    a6.axvline(T['bpm'], color='tab:green', lw=2, label=f'tracker picks: {T["bpm"]:.0f} bpm')
    a6.set(title='6. Motion bins (and 0.5x/2x/3x) zeroed (red); tracker chooses',
           xlabel='frequency (bpm)', ylabel='power')
    a6.legend(loc='upper right', fontsize=8)

    # 7. tracker scores
    a7 = ax[3, 0]
    ts = np.array([d['t'] for d in ticks])
    S = np.array([d['share'] for d in ticks]).T
    cand = (K_LO + np.arange(S.shape[0])) * BIN_BPM
    a7.pcolormesh(ts, cand, S, shading='nearest', cmap='magma')
    a7.plot(ts, [d['naive'] for d in ticks], 'o', ms=3, color='tab:cyan', label='biggest bin each second')
    a7.plot(ts, [d['bpm'] for d in ticks], '-', lw=2, color='lime', label='tracker path')
    a7.axvline(T['t'], color='w', ls=':')
    a7.set(title='7. Each second (x): share of power at each candidate bpm (bright = strong)',
           xlabel='time (s)', ylabel='bpm')
    a7.legend(loc='upper right', fontsize=8)

    # 8. bpm over time
    a8 = ax[3, 1]
    a8.plot(ts, [d['naive'] for d in ticks], 'o--', ms=3, lw=0.8, color='0.5', label='plain biggest bin')
    a8.plot(ts, [d['bpm'] for d in ticks], '-', lw=2, color='tab:green', label='tracker (firmware output)')
    a8.axvline(T['t'], color='tab:orange', ls=':')
    a8.set(title=f'8. Output: tracker can only move ~{SIGMA_BPM:g} bpm/s, so stray peaks are ignored',
           xlabel='time (s)', ylabel='bpm')
    a8.legend(loc='upper right', fontsize=8)

    # Same frequency axis on every spectrum panel, with the candidate band shaded.
    for a in (a4, a5, a6):
        if a.axison:
            a.set_xlim(0, KM_HI * BIN_BPM)
            if a is not a4:
                a.axvspan(K_LO * BIN_BPM, K_HI * BIN_BPM, color='tab:green', alpha=0.08)

    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(args.out, dpi=110)
    print(f'wrote {args.out}  (detail at t={T["t"]:.0f} s: tracker {T["bpm"]:.1f} bpm, '
          f'biggest bin {T["naive"]:.1f} bpm, conf {T["conf"]:.2f})')
    if args.show:
        plt.show()


if __name__ == '__main__':
    main()
