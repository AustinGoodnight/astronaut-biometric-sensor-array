#!/usr/bin/env python3
"""Step-by-step picture of the classic heart-rate path (src/max/classic_hr.c), with a
side-by-side against the spectral tracker on the same input.

A Python port of the firmware's per-sample pipeline:

  raw PPG -> step-artifact pause -> DC tracker -> 0.7-3.5 Hz bandpass
  -> 25 Hz window (8 s) -> once a second: autocorrelation, first strong peak
  -> median of the last 5; "--" after 5 s without a good window

Classic doesn't remove motion (the spectral tracker does). Not ported: the sample-rate
refinement (offline it's exactly 100 Hz).

    python3 scripts/classic_hr_viz.py ../hr-max30102-ble/samples.csv --at 22
    python3 scripts/classic_hr_viz.py rec.csv --fake-motion 1.3 --fake-amp 0.5
"""

import argparse
import math
import sys

import numpy as np
import matplotlib.pyplot as plt

import spectral_hr_viz as spec

FS = 100.0
WIN_DECIM, WIN_LEN, MAX_LAG = 4, 200, 40
FS_WIN = FS / WIN_DECIM
LAG_MIN = max(2, int(FS_WIN * 60 / 220))
LAG_MAX = min(MAX_LAG - 1, int(FS_WIN * 60 / 42 + 0.5))
AC_MIN_CORR = 0.50
PEAK_FRAC = 0.85
HR_HIST, MAX_AGE_SEC = 5, 5
DC_ALPHA = 0.01
STEP_FRAC, PAUSE_STEP_SEC, PAUSE_START_SEC = 0.03, 1.5, 2.0


class Bandpass:
    """bandpass.h: two TDF-II biquads, starting from 0 (not from the first sample)."""

    def __init__(self):
        self.z = [[0.0, 0.0], [0.0, 0.0]]

    def reset(self):
        self.z = [[0.0, 0.0], [0.0, 0.0]]

    def __call__(self, x):
        for (b0, b1, b2, a1, a2), z in zip(spec.BP, self.z):
            y = b0 * x + z[0]
            z[0] = b1 * x - a1 * y + z[1]
            z[1] = b2 * x - a2 * y
            x = y
        return x


def estimate_hr(win):
    """estimate(): returns (ok, bpm, rmax, ac[lag], best_lag)."""
    x = np.asarray(win, float)
    x = x - x.mean()
    n = len(x)
    r0 = (x * x).sum() / n
    ac = np.full(MAX_LAG + 2, np.nan)
    if r0 <= 0:
        return False, None, 0.0, ac, None
    for lag in range(LAG_MIN - 1, LAG_MAX + 2):
        ac[lag] = ((x[:n - lag] * x[lag:]).sum() / (n - lag)) / r0
    rmax = max(0.0, np.nanmax(ac[LAG_MIN:LAG_MAX + 1]))
    if rmax < AC_MIN_CORR:
        return False, None, rmax, ac, None
    best = None
    for lag in range(LAG_MIN, LAG_MAX + 1):
        if ac[lag] >= ac[lag - 1] and ac[lag] >= ac[lag + 1] and ac[lag] >= PEAK_FRAC * rmax:
            best = lag
            break
    if best is None:
        return False, None, rmax, ac, None
    a, b, c = ac[best - 1], ac[best], ac[best + 1]
    d = a - 2 * b + c
    delta = 0.5 * (a - c) / d if d != 0 else 0.0
    delta = min(1.0, max(-1.0, delta))
    return True, 60 * FS_WIN / (best + delta), rmax, ac, best


def run(ppg):
    """classic_hr_add_sample() + classic_hr_update() over a recording. Returns (per-sample trace, ticks)."""
    n = len(ppg)
    tr = {'f': np.full(n, np.nan), 'pause': np.zeros(n, bool)}

    filt = Bandpass()
    idx = 0
    ir_dc = prev = float(ppg[0])
    pause_until = int(PAUSE_START_SEC * FS)
    win, dec_acc, dec_n = [], 0.0, 0
    last_tick = 0
    hist, valid, est, last_good = [], False, 0.0, 0
    ticks = []

    for i in range(n):
        ir = float(ppg[i])
        idx += 1
        if abs(ir - prev) > STEP_FRAC * ir_dc:                      # step artifact
            if abs(ir - prev) > 0.10 * ir_dc:
                valid, hist = False, []
            filt.reset()
            ir_dc = ir
            win, dec_acc, dec_n = [], 0.0, 0
            pause_until = idx + int(PAUSE_STEP_SEC * FS)
        prev = ir

        x = ir - ir_dc
        ir_dc += DC_ALPHA * x
        f = filt(x)
        tr['f'][i] = f
        tr['pause'][i] = idx < pause_until

        if idx >= pause_until:
            dec_acc += f
            dec_n += 1
            if dec_n >= WIN_DECIM:
                win.append(dec_acc / WIN_DECIM)
                win = win[-WIN_LEN:]
                dec_acc, dec_n = 0.0, 0

        if idx - last_tick >= int(FS):                              # classic_hr_update()
            last_tick = idx
            T = dict(t=idx / FS, warm=len(win) < WIN_LEN, ok=False, raw=None,
                     rmax=0.0, ac=None, lag=None, win=None)
            if len(win) >= WIN_LEN:
                ok, bpm, rmax, ac, lag = estimate_hr(win)
                T.update(ok=ok, raw=bpm, rmax=rmax, ac=ac, lag=lag, win=np.array(win))
                if ok:
                    hist = (hist + [bpm])[-HR_HIST:]
                    est = sorted(hist)[len(hist) // 2]
                    valid, last_good = True, idx
            if valid and idx - last_good >= MAX_AGE_SEC * FS:
                valid, hist = False, []
            T['out'] = est if valid else None
            ticks.append(T)
    return tr, ticks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv')
    ap.add_argument('--at', type=float, help='second to explain in detail (default: middle)')
    ap.add_argument('--fake-motion', type=float, metavar='HZ',
                    help='add a synthetic arm swing at this rate (same as spectral_hr_viz.py)')
    ap.add_argument('--fake-amp', type=float, default=1.0,
                    help='size of the synthetic swing, as a multiple of the pulse (default 1)')
    ap.add_argument('-o', '--out', default='classic_hr_viz.png')
    ap.add_argument('--show', action='store_true', help='open an interactive window too')
    args = ap.parse_args()

    ppg, acc = spec.load(args.csv)
    if args.fake_motion:
        ppg, acc = spec.add_fake_motion(ppg, args.fake_motion, args.fake_amp)

    tr, ticks = run(ppg)
    _, sticks = spec.run(ppg, acc)

    est_ticks = [d for d in ticks if d['ac'] is not None]
    if not est_ticks:
        sys.exit('no full 8 s window in this recording')
    at = args.at if args.at is not None else est_ticks[len(est_ticks) // 2]['t']
    T = min(est_ticks, key=lambda d: abs(d['t'] - at))
    t0 = T['t'] - WIN_LEN / FS_WIN
    t = np.arange(len(ppg)) / FS

    fig, ax = plt.subplots(4, 2, figsize=(15, 15))
    title = f'Classic HR (autocorrelation) on {args.csv}'
    if args.fake_motion:
        title += (f'   [SYNTHETIC {args.fake_motion:g} Hz ({args.fake_motion * 60:.0f}/min) '
                  f'arm swing added, {args.fake_amp:g}x pulse]')
    fig.suptitle(title, fontsize=13, weight='bold')

    def shade(a, mask, color, label):
        on = False
        for i, m in enumerate(np.append(mask, False)):
            if m and not on:
                s, on = i, True
            elif not m and on:
                a.axvspan(t[s], t[i - 1], color=color, alpha=0.25, lw=0, label=label)
                label, on = None, False

    # 1. raw + holdoffs
    a = ax[0, 0]
    a.plot(t, ppg, lw=0.6, color='0.3')
    shade(a, tr['pause'], 'tab:red', 'paused (start / step artifact): window cleared')
    a.axvspan(t0, T['t'], color='tab:orange', alpha=0.2, label=f'8 s window ending at {T["t"]:.0f} s')
    a.set(title='1. Raw PPG (100 Hz); a jump > 3% of DC pauses everything 1.5 s',
          xlabel='time (s)', ylabel='ADC counts')
    a.legend(loc='upper left', fontsize=8)

    # 2. filtered
    a = ax[0, 1]
    a.plot(t, tr['f'], lw=0.8, color='tab:blue', label='bandpassed')
    a.set_title('2. DC removed + bandpass 0.7-3.5 Hz (42-210 bpm)')
    a.axvspan(t0, T['t'], color='tab:orange', alpha=0.2)
    a.set(xlabel='time (s)')
    a.legend(loc='upper left', fontsize=8)

    # 3. window zoom
    a = ax[1, 0]
    m = (t > t0) & (t <= T['t'])
    a.plot(t[m], tr['f'][m], lw=1.2, color='tab:blue', label='bandpassed, 100 Hz')
    a.set_title('3. The 8 s window, zoomed: one bump per beat')
    a.set(xlabel='time (s)')
    a.legend(loc='upper right', fontsize=8)

    # 4. window + itself shifted by one beat
    a = ax[1, 1]
    w = T['win'] - T['win'].mean()
    tw = t0 + np.arange(WIN_LEN) / FS_WIN
    a.plot(tw, w, color='tab:blue', lw=1.2, label='window (25 Hz, mean removed)')
    if T['lag'] is not None:
        a.plot(tw + T['lag'] / FS_WIN, w, color='tab:green', lw=1, alpha=0.8,
               label=f'same, shifted {T["lag"]} samples = {T["lag"] / FS_WIN:.2f} s')
        a.set_title('4. Shift the window by one beat and it lines up with itself')
    else:
        a.set_title('4. The window (no lag lined up well enough this second)')
    a.set_xlim(tw[0], tw[-1])
    a.set(xlabel='time (s)')
    a.legend(loc='upper right', fontsize=8)

    # 5. autocorrelation
    a = ax[2, 0]
    lags = np.arange(LAG_MIN, LAG_MAX + 1)
    a.plot(lags, T['ac'][lags], 'o-', ms=3, color='tab:blue', label='how well it lines up at each shift')
    a.axhline(AC_MIN_CORR, color='tab:red', ls='--', lw=1, label=f'must beat {AC_MIN_CORR} somewhere')
    a.axhline(PEAK_FRAC * T['rmax'], color='0.5', ls=':', lw=1,
              label=f'{PEAK_FRAC:.0%} of best: first peak above this wins')
    gl = lags[np.nanargmax(T['ac'][lags])]
    a.axvline(gl, color='0.5', lw=1)
    if T['lag'] is not None:
        a.axvline(T['lag'], color='tab:green', lw=2, label=f'chosen: lag {T["lag"]} -> {T["raw"]:.0f} bpm')
    a.set(xlabel='shift (samples at 25 Hz)', ylabel='correlation (1 = perfect)', ylim=(-1, 1.05),
          title=f'5. Autocorrelation at t={T["t"]:.0f} s (best {T["rmax"]:.2f} at lag {gl})')
    top = a.secondary_xaxis('top', functions=(lambda L: 60 * FS_WIN / np.maximum(L, 1e-3),
                                             lambda B: 60 * FS_WIN / np.maximum(B, 1e-3)))
    top.set_xlabel('= bpm')
    top.set_xticks([200, 150, 120, 100, 80, 60, 50, 42])
    a.legend(loc='lower right', fontsize=8)

    # 6. publish rules
    a = ax[2, 1]
    tt = np.array([d['t'] for d in ticks])
    ok = [d for d in ticks if d['ok']]
    fail = [d for d in ticks if d['ac'] is not None and not d['ok']]
    a.plot([d['t'] for d in ok], [d['raw'] for d in ok], 'o', ms=4, color='tab:blue',
           label='window estimate (accepted)')
    out = [d['out'] if d['out'] is not None else np.nan for d in ticks]
    a.plot(tt, out, '-', lw=2, color='tab:green', label='published (median of last 5)')
    lo = np.nanmin(out + [d['raw'] for d in ok] + [60]) - 5
    a.plot([d['t'] for d in fail], [lo] * len(fail), 'x', color='tab:red', ms=6,
           label='failed (not periodic enough)')
    a.axvline(T['t'], color='tab:orange', ls=':')
    a.set(title='6. Each second: estimate -> median of last 5 -> published (-- after 5 s of failures)',
          xlabel='time (s)', ylabel='bpm')
    a.legend(loc='best', fontsize=8)

    # 7. autocorrelation over time
    a = ax[3, 0]
    full = [d for d in ticks if d['ac'] is not None]
    if full:
        A = np.array([d['ac'][lags] for d in full]).T
        a.pcolormesh([d['t'] for d in full], 60 * FS_WIN / lags, A, shading='nearest',
                     cmap='magma', vmin=0, vmax=1)
        a.plot([d['t'] for d in full if d['ok']], [d['raw'] for d in full if d['ok']], 'o',
               ms=3, color='tab:cyan', label='window estimate')
        a.plot(tt, out, '-', lw=2, color='lime', label='published')
    a.set_ylim(40, 180)
    a.axvline(T['t'], color='w', ls=':')
    a.set(title='7. Each second (x): correlation at each candidate bpm (bright = lines up well)',
          xlabel='time (s)', ylabel='bpm')
    a.legend(loc='upper right', fontsize=8)

    # 8. head-to-head
    a = ax[3, 1]
    a.plot(tt, out, '-', lw=2, color='tab:blue', label='classic')
    a.plot([d['t'] for d in sticks], [d['bpm'] for d in sticks], '-', lw=2, color='tab:green',
           label='spectral tracker')
    a.axvline(T['t'], color='tab:orange', ls=':')
    a.set(title='8. Classic vs. spectral on the same input (gaps = classic shows "--")',
          xlabel='time (s)', ylabel='bpm')
    a.legend(loc='best', fontsize=8)

    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(args.out, dpi=110)
    print(f'wrote {args.out}')
    if args.show:
        plt.show()


if __name__ == '__main__':
    main()
