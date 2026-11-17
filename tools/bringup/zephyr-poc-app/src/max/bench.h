#ifndef BENCH_H
#define BENCH_H

/* USB bench mode: the serial protocol the testing pages speak (tools/hr-algo-compare,
 * and the ESP32-era HR testing page), on the USB console.
 *
 * Host -> board, single characters:
 *   R / r     raw stream on / off (also off when the host drops DTR). While on, the
 *             normal Zephyr log is muted and only bench lines go out.
 *   L<0-255>  LED current, ended by any non-digit (e.g. "L80\n")
 *   C<1-3>    LED in slot 1: 1 red, 2 IR, 3 green (restarts the session)
 *   S / s     publish the spectral / classic heart rate
 *
 * Board -> host, one line each:
 *   raw,<seq>,<ppg>:<slot2>,...          24 samples per line
 *   mc,<seq>,<ppg>:<ppg>:<ax>:<ay>:<az>,...
 *                                       same 24 samples, same <seq> (Sense only): the
 *                                       classic path's bandpassed PPG in hundredths of an
 *                                       ADC count (twice: the format once carried it
 *                                       before/after motion cancelling, and the pages
 *                                       still parse it that way), and the accel in
 *                                       hundredths of m/s^2 (0:0:0 = no reading)
 *   anything from bench_log() (status:, LED changes, ...) */

#include <stdbool.h>
#include <stdint.h>

struct bench_handlers {
    void (*set_led)(uint8_t led);           /* C<n> */
    void (*set_led_current)(uint8_t pa);    /* L<n> */
    void (*set_spectral)(bool on);          /* S / s */
};

/* Commands are dispatched to these from bench_poll(). */
void bench_init(const struct bench_handlers *h);

/* Handle pending commands from the host. Call regularly (each sensor poll). */
void bench_poll(void);

/* A plain line on USB in bench mode, a normal log line otherwise. */
void bench_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Queue one sample for the raw / mc lines; each sends a line every 24 calls (only
 * while bench mode is on, but the sequence number always advances). Call
 * bench_send_raw() first for each sample, then bench_send_mc() for the same sample.
 * acc may be NULL (no IMU reading). */
void bench_send_raw(uint32_t ppg, uint32_t slot2);
void bench_send_mc(float ppg, const float *acc);

#endif /* BENCH_H */
