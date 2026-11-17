/* Radix-2 FFT. See fft.h for what goes in and comes out.
 *
 * The trick: a DFT of n samples can be built from two DFTs of n/2 samples, one of the
 * even-indexed samples (E) and one of the odd-indexed samples (O):
 *
 *     X[k]       = E[k] + w^k * O[k]
 *     X[k + n/2] = E[k] - w^k * O[k]          w = e^(-2*pi*i / n), a "twiddle factor"
 *
 * Split those halves again and again and you get down to DFTs of 1 sample, which are
 * just the sample itself. This code does it bottom-up:
 *   1. reorder the samples so that every group that needs combining sits side by side
 *   2. combine neighbours into DFTs of 2, then pairs of those into DFTs of 4, ... up to n
 * n = 1024 is 10 combining passes of 512 combines each: ~10k operations instead of ~1M. */

#include "fft.h"

#include <math.h>

#define FFT_PI 3.14159265358979f

void fft(float *re, float *im, int n)
{
    /* ---- 1. Bit-reversal reorder ----
     * Repeatedly splitting into evens/odds leaves sample t at the position whose binary
     * index is t's bits backwards. For n = 8 (3 bits):
     *     index  0   1   2   3   4   5   6   7
     *     binary 000 001 010 011 100 101 110 111
     *     gets   0   4   2   6   1   5   3   7      (001 <-> 100, 011 <-> 110, ...)
     * i counts normally, j counts in reversed binary (j = reverse(i)) at every step. */
    for (int i = 1, j = 0; i < n; i++) {
        /* Add 1 to j, but with the carry going from the top bit downwards: clear the top
         * bits that are 1 (they'd carry), then set the first one that's 0. */
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) { /* swap each pair once (i > j was already swapped earlier) */
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    /* ---- 2. Combining passes ("butterflies") ----
     * Before the pass for size len, the array is blocks of len/2 values, each already
     * a finished DFT. Each block of len is a left half E and a right half O; combine them
     * into one DFT of len with the formula at the top of this file. */
    for (int len = 2; len <= n; len <<= 1) {
        /* w = e^(-2*pi*i/len) = cos(ang) + i*sin(ang): one step around the unit circle,
         * 1/len of a full turn. */
        float ang = -2.0f * FFT_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);

        for (int i = 0; i < n; i += len) { /* each block of len values */
            /* (cr, ci) = w^k, starting at w^0 = 1 */
            float cr = 1.0f, ci = 0.0f;

            for (int k = 0; k < len / 2; k++) {
                int a = i + k;           /* E[k], in the left half */
                int b = a + len / 2;     /* O[k], in the right half */

                /* (tr, ti) = w^k * O[k]  (complex multiply: (x+iy)(u+iv) = xu-yv + i(xv+yu)) */
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;

                /* X[k + len/2] = E[k] - w^k*O[k]  and  X[k] = E[k] + w^k*O[k], written over
                 * the two inputs (each pair is only needed for these two outputs) */
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;

                /* w^(k+1) = w^k * w: rotate one more step instead of calling cos/sin again */
                float ncr = cr * wr - ci * wi;

                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}
