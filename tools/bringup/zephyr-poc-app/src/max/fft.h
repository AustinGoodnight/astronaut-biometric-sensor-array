#ifndef FFT_H
#define FFT_H

/* In-place complex FFT (radix-2 Cooley-Tukey). n must be a power of 2.
 *
 * Input:  re[0..n-1] = the signal, im[0..n-1] = 0 for a real signal.
 * Output: bin k holds how much of a wave at frequency k * fs / n is in the signal:
 *         re[k] = sum x[t]*cos(2*pi*k*t/n), im[k] = -sum x[t]*sin(2*pi*k*t/n).
 *         For a real signal only bins 0..n/2 are useful; bins above n/2 mirror them.
 *
 * Same result as that double sum (n*n multiplies), in n*log2(n). No dependencies, so it
 * also builds on a host. */
void fft(float *re, float *im, int n);

#endif /* FFT_H */
