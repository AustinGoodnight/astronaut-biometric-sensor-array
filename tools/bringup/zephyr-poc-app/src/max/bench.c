/* USB bench protocol. See bench.h. */

#include "bench.h"

#include <stdarg.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>

/* Same log module as main.c, so log lines keep their "blinky_main:" prefix. */
LOG_MODULE_DECLARE(blinky_main, LOG_LEVEL_INF);

#define BATCH 24
#define MC_AXES 3

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct bench_handlers *handlers;
static bool active; /* set by 'R', cleared by 'r' or DTR dropping */

static uint32_t rawPpg[BATCH], rawSlot2[BATCH];
static uint8_t rawCount;
static uint16_t rawSeq;

static int16_t mcPre[BATCH], mcPost[BATCH], mcAcc[BATCH][MC_AXES];
static uint8_t mcCount;

void bench_init(const struct bench_handlers *h)
{
    handlers = h;
}

static void write_bytes(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uart_poll_out(uart, s[i]); /* CDC ACM drops (never blocks) if the host stalls */
    }
}

static void set_active(bool on)
{
    const struct log_backend *backend = log_backend_get_by_name("log_backend_uart");

    if (on == active) {
        return;
    }
    if (!on) {
        active = false;
    }
    if (backend != NULL) {
        if (on) {
            log_backend_disable(backend);
        } else {
            log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
        }
    }
    if (on) {
        active = true;
    } else {
        LOG_INF("USB bench mode off, normal logging resumed");
    }
}

void bench_log(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(line)) {
        n = sizeof(line) - 1;
    }
    if (active) {
        write_bytes(line, n);
        write_bytes("\r\n", 2);
    } else {
        LOG_INF("%s", line);
    }
}

void bench_send_raw(uint32_t ppg, uint32_t slot2)
{
    rawPpg[rawCount] = ppg;
    rawSlot2[rawCount] = slot2;
    if (++rawCount < BATCH) {
        return;
    }
    rawCount = 0;
    uint16_t seq = rawSeq++;

    if (!active) {
        return;
    }
    char line[16 + BATCH * 16];
    int n = snprintf(line, sizeof(line), "raw,%u", (unsigned)seq);

    for (int i = 0; i < BATCH && n < (int)sizeof(line) - 16; i++) {
        n += snprintf(line + n, sizeof(line) - n, ",%lu:%lu",
                      (unsigned long)rawPpg[i], (unsigned long)rawSlot2[i]);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    write_bytes(line, n);
}

static int16_t to_i16(float v)
{
    return (int16_t)CLAMP(v * 100.0f, -32767.0f, 32767.0f);
}

void bench_send_mc(float ppg, const float *acc)
{
    mcPre[mcCount] = to_i16(ppg);
    mcPost[mcCount] = mcPre[mcCount];
    for (int a = 0; a < MC_AXES; a++) {
        mcAcc[mcCount][a] = acc != NULL ? to_i16(acc[a]) : 0;
    }
    if (++mcCount < BATCH) {
        return;
    }
    mcCount = 0;
    if (!active) {
        return;
    }
    /* bench_send_raw() ran first for every sample, so this batch covers the same
     * samples as the raw line just sent. */
    static char line[16 + BATCH * 34]; /* static: too big for the work queue stack */
    int n = snprintf(line, sizeof(line), "mc,%u", (unsigned)(uint16_t)(rawSeq - 1));

    for (int i = 0; i < BATCH && n < (int)sizeof(line) - 34; i++) {
        n += snprintf(line + n, sizeof(line) - n, ",%d:%d:%d:%d:%d", mcPre[i], mcPost[i],
                      mcAcc[i][0], mcAcc[i][1], mcAcc[i][2]);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    write_bytes(line, n);
}

void bench_poll(void)
{
    static bool numParsing, ledParsing;
    static int num;
    unsigned char c;
    uint32_t dtr = 0;

    while (uart_poll_in(uart, &c) == 0) {
        if (ledParsing) { /* the one character after 'C' */
            ledParsing = false;
            if (c >= '1' && c <= '3') {
                handlers->set_led(c - '0');
            }
        } else if (c == 'C') {
            ledParsing = true;
        } else if (c == 'S' || c == 's') {
            handlers->set_spectral(c == 'S');
        } else if (c == 'L') {
            numParsing = true;
            num = 0;
        } else if (numParsing && c >= '0' && c <= '9') {
            if (num < 1000) {
                num = num * 10 + (c - '0');
            }
        } else if (numParsing) { /* any non-digit terminates the number */
            numParsing = false;
            handlers->set_led_current((uint8_t)MIN(num, 255));
        } else if (c == 'R') {
            set_active(true);
        } else if (c == 'r') {
            set_active(false);
        }
    }

    /* Page closed without sending 'r': the host dropped DTR. */
    if (active && uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) == 0 && !dtr) {
        set_active(false);
    }
}
