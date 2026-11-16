#!/usr/bin/env python3
"""Step-by-step picture of the classic heart-rate path (src/max/hr_main.c), with a
side-by-side against the spectral tracker on the same input.

A Python port of the firmware's per-sample pipeline:

  raw IR -> step-artifact detector (holdoff) -> DC tracker -> 0.7-3.5 Hz bandpass
  -> NLMS motion cancelling (accelerometer as reference, auto-bypass)
  -> 25 Hz window (8 s) -> once a second: autocorrelation, first strong peak
  -> publish rules (15 bpm jump guard, median of 5, clear after 5 failures / 15 s)

Not ported: the edge/beat markers (log only, they don't change the published bpm),
the Red channel (the firmware feeds the slot-1 LED in as both channels), and the
sample-rate refinement (offline it's exactly 100 Hz).

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
HR_JUMP_BPM, HR_JUMP_CONFIRM, HR_FAIL_TICKS, HR_HIST, HR_STALE_SEC = 15.0, 3, 5, 5, 15.0
DC_ALPHA = 0.01
STEP_FRAC, HOLDOFF_STEP_SEC, SETTLE_SEC = 0.03, 1.5, 2.0

MC_TAPS, MC_AXES = 8, 3
MC_N = MC_TAPS * MC_AXES
MC_MU, MC_REST, MC_LEAK = 0.05, 0.05, 0.9995
MC_EPS = MC_N * MC_REST * MC_REST
MC_BYPASS_ON, MC_BYPASS_OFF = 1.0, 0.9


class Bandpass:
    """PPG_FILTER_INIT + filter_process(): two TDF-II biquads, not primed."""

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
    """estimateHr(): returns (ok, bpm, rmax, ac[lag], best_lag)."""
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


def run(ppg, acc, mc_on=True):
    """processSample() + onTick() over a recording. Returns (per-sample trace, ticks)."""
    n = len(ppg)
    tr = {k: np.full(n, np.nan) for k in ('before', 'after', 'est')}
    tr['holdoff'] = np.zeros(n, bool)
    tr['bypass'] = np.zeros(n, bool)

    filt = Bandpass()
    acc_f = [Bandpass() for _ in range(MC_AXES)]
    acc_dc = np.zeros(MC_AXES)
    ref = np.zeros((MC_AXES, MC_TAPS))
    w = np.zeros((MC_AXES, MC_TAPS))
    mc_primed = False
    in_pow = out_pow = 0.0
    bypass = False

    idx = 0
    need_init = True
    ir_dc = prev = 0.0
    holdoff_until = 0
    win, dec_acc, dec_n = [], 0.0, 0
    last_tick = 0
    hist, pend_bpm, pend_n = [], 0.0, 0
    hr_valid, hr_est, last_hr = False, 0.0, 0
    fail = 0
    ticks = []

    def clear():
        nonlocal hr_valid, hist, pend_n
        hr_valid, hist, pend_n = False, [], 0

    for i in range(n):
        ir = float(ppg[i])
        idx += 1
        if need_init:
            need_init = False
            ir_dc = prev = ir
            holdoff_until = int(SETTLE_SEC * FS)
        if abs(ir - prev) > STEP_FRAC * ir_dc:                      # step artifact
            if abs(ir - prev) > 0.10 * ir_dc and hr_valid:
                clear()
            filt.reset()
            ir_dc = ir
            win, dec_acc, dec_n = [], 0.0, 0
            holdoff_until = idx + int(HOLDOFF_STEP_SEC * FS)
        prev = ir

        x = ir - ir_dc
        ir_dc += DC_ALPHA * x
        f = filt(x)
        tr['before'][i] = f

        if mc_on and acc is not None:
            live = idx >= holdoff_until
            a_now = acc[i]
            if not mc_primed:
                acc_dc[:] = a_now
                mc_primed = True
            for a in range(MC_AXES):                                # mc_push_ref()
                d = a_now[a] - acc_dc[a]
                acc_dc[a] += 0.01 * d
                ref[a, 1:] = ref[a, :-1].copy()
                ref[a, 0] = acc_f[a](d)
            ref_pow = (ref * ref).sum()
            y = (w * ref).sum()                                     # mc_cancel()
            e = f - y
            if live:
                w = MC_LEAK * w + (MC_MU * e / (MC_EPS + ref_pow)) * ref
                in_pow += 0.01 * (f * f - in_pow)
                out_pow += 0.01 * (e * e - out_pow)
                if out_pow > MC_BYPASS_ON * in_pow:
                    bypass = True
                elif out_pow < MC_BYPASS_OFF * in_pow:
                    bypass = False
                if not bypass:
                    f = e
            tr['est'][i] = y
            tr['bypass'][i] = bypass
        tr['after'][i] = f
        tr['holdoff'][i] = idx < holdoff_until

        if idx >= holdoff_until:
            dec_acc += f
            dec_n += 1
            if dec_n >= WIN_DECIM:
                win.append(dec_acc / WIN_DECIM)
                win = win[-WIN_LEN:]
                dec_acc, dec_n = 0.0, 0

        if idx - last_tick >= int(FS):                              # onTick()
            last_tick = idx
            if hr_valid and idx - last_hr > HR_STALE_SEC * FS:
                clear()
            T = dict(t=idx / FS, warm=len(win) < WIN_LEN, ok=False, raw=None, held=False,
                     rmax=0.0, ac=None, lag=None, win=None)
            if len(win) >= WIN_LEN:
                ok, bpm, rmax, ac, lag = estimate_hr(win)
                T.update(ok=ok, raw=bpm, rmax=rmax, ac=ac, lag=lag, win=np.array(win))
                if ok:                                              # publishHr()
                    fail = 0
                    adopt = True
                    if hr_valid and abs(bpm - hr_est) > HR_JUMP_BPM:
                        pend_n = pend_n + 1 if pend_n > 0 and abs(bpm - pend_bpm) <= 10 else 1
                        pend_bpm = bpm
                        if pend_n < HR_JUMP_CONFIRM:
                            adopt = False
                            T['held'] = True
                        else:
                            hist = []
                    if adopt:
                        pend_n = 0
                        hist = (hist + [bpm])[-HR_HIST:]
                        hr_est = sorted(hist)[len(hist) // 2]
                        hr_valid, last_hr = True, idx
                else:
                    fail = min(fail + 1, 255)
                    if fail >= HR_FAIL_TICKS and hr_valid:
                        clear()
            T['out'] = hr_est if hr_valid else None
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

    tr, ticks = run(ppg, acc, mc_on=True)
    _, ticks_nomc = run(ppg, acc, mc_on=False) if acc is not None else (None, None)
    _, sticks = spec.run(ppg, acc)

    est_ticks = [d for d in ticks if d['ac'] is not None]
    if not est_ticks:
        sys.exit('no full 8 s window in this recording')
    at = args.at if args.at is not None else est_ticks[len(est_ticks) // 2]['t']
    T = min(est_ticks, key=lambda d: abs(d['t'] - at))
    t0 = T['t'] - WIN_LEN / FS_WIN
    t = np.arange(len(ppg)) / FS
    has_mc = acc is not None

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
    shade(a, tr['holdoff'], 'tab:red', 'holdoff (start / step artifact): window cleared')
    a.axvspan(t0, T['t'], color='tab:orange', alpha=0.2, label=f'8 s window ending at {T["t"]:.0f} s')
    a.set(title='1. Raw PPG (IR, 100 Hz); a jump > 3% of DC pauses everything 1.5 s',
          xlabel='time (s)', ylabel='ADC counts')
    a.legend(loc='upper left', fontsize=8)

    # 2. filtered, before/after MC
    a = ax[0, 1]
    if has_mc:
        a.plot(t, tr['before'], lw=0.6, color='0.7', label='bandpassed (before motion cancel)')
        a.plot(t, tr['after'], lw=0.8, color='tab:blue', label='after motion cancel')
        shade(a, tr['bypass'] & ~tr['holdoff'], 'tab:purple', 'bypassed (cancelling made it worse)')
        a.set_title('2. DC removed + bandpass 0.7-3.5 Hz, then NLMS motion cancelling')
    else:
        a.plot(t, tr['after'], lw=0.8, color='tab:blue', label='bandpassed')
        a.set_title('2. DC removed + bandpass 0.7-3.5 Hz (no accelerometer: no cancelling)')
    a.axvspan(t0, T['t'], color='tab:orange', alpha=0.2)
    a.set(xlabel='time (s)')
    a.legend(loc='upper left', fontsize=8)

    # 3. MC zoom
    a = ax[1, 0]
    m = (t > t0) & (t <= T['t'])
    if has_mc:
        a.plot(t[m], tr['before'][m], lw=1, color='0.6', label='before')
        a.plot(t[m], tr['est'][m], lw=1, color='tab:red', label='motion estimate (from accelerometer)')
        a.plot(t[m], tr['after'][m], lw=1.4, color='tab:blue', label='after = before - estimate')
        a.set_title('3. Motion cancel, zoomed to the window: learns accel -> PPG, subtracts it')
    else:
        a.plot(t[m], tr['after'][m], lw=1.2, color='tab:blue', label='bandpassed')
        a.set_title('3. Window, zoomed (nothing subtracted: no accelerometer)')
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
    ok = [d for d in ticks if d['ok'] and not d['held']]
    held = [d for d in ticks if d['held']]
    fail = [d for d in ticks if d['ac'] is not None and not d['ok']]
    a.plot([d['t'] for d in ok], [d['raw'] for d in ok], 'o', ms=4, color='tab:blue',
           label='window estimate (accepted)')
    a.plot([d['t'] for d in held], [d['raw'] for d in held], 's', ms=5, color='tab:orange',
           label='held back: >15 bpm jump, not yet repeated 3x')
    out = [d['out'] if d['out'] is not None else np.nan for d in ticks]
    a.plot(tt, out, '-', lw=2, color='tab:green', label='published (median of last 5)')
    lo = np.nanmin(out + [d['raw'] for d in ok + held] + [60]) - 5
    a.plot([d['t'] for d in fail], [lo] * len(fail), 'x', color='tab:red', ms=6,
           label='failed (not periodic enough)')
    a.axvline(T['t'], color='tab:orange', ls=':')
    a.set(title='6. Each second: estimate -> jump guard -> median -> published',
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
    a.plot(tt, out, '-', lw=2, color='tab:blue', label='classic' + (' (motion cancel on)' if has_mc else ''))
    if ticks_nomc is not None:
        a.plot(tt, [d['out'] if d['out'] is not None else np.nan for d in ticks_nomc], '--', lw=1.2,
               color='tab:blue', alpha=0.6, label='classic, motion cancel off')
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
