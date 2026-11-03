/* Shared helpers for the session tests. */
#ifndef A52_TEST_UTIL_H
#define A52_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "a52session.h"
#include "a52debug.h"
#include "test_tmpdir.h"

static inline void test_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* FujiNet's runtime captures stderr into its log: give its log thread a
   moment to pass the message on before exiting. */
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); \
    test_sleep_ms(500); _exit(1); } } while (0)

/* A session in a fresh XDG tree. With FujiNet, its own fnconfig.ini: the
   SD card and no network hosts (tests never touch real servers). */
static inline a52session *test_session_ex(const char *tag, int fujinet)
{
    char base[512], cfg[600], data[600], path[700];
    a52session_paths p;
    FILE *fp;
    test_tmpdir(base, sizeof base, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    if (!fujinet)
        p.fujinet_lib = "";
    else {
        test_mkdir(data);
        snprintf(path, sizeof path, "%s/fujinet", data);
        test_mkdir(path);
        snprintf(path, sizeof path, "%s/fujinet/fnconfig.ini", data);
        if ((fp = fopen(path, "w")) != NULL) {
            fputs("[General]\ndevicename=fujinet-test\nconfig_enabled=1\nboot_mode=0\n"
                  "[Host1]\ntype=SD\nname=SD\n", fp);
            fclose(fp);
        }
    }
    return a52session_new(&p);
}

static inline a52session *test_session(const char *tag)
{
    return test_session_ex(tag, 0);
}

/* Starts the console, the cartridge in sync mode (deterministic), no audio,
   no gamepads. cart: a game to boot instead of CONFIG, or NULL. */
static inline void test_start_cart(a52session *s, int fujinet, const char *cart)
{
    a52session_start_opts o;
    int rc;
    a52session_default_opts(s, &o);
    o.enable_fujinet = fujinet;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.sync_cart = 1;
    o.cart_path = cart ? cart : "";
    rc = a52session_start(s, &o);
    CHECK(rc == 0, "start: %s", a52session_last_error(s));
}

static inline void test_start(a52session *s, int fujinet)
{
    test_start_cart(s, fujinet, NULL);
}

/* Starts with the built-in FujiNet and waits for the cartridge's link; 0 if
   there is no runtime. The cartridge runs on its worker thread, as for real. */
static inline int test_start_fujinet(a52session *s)
{
    a52session_start_opts o;
    int t;
    a52session_default_opts(s, &o);
    o.enable_fujinet = 1;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.sync_cart = 0;
    o.cart_path = "";
    if (a52session_start(s, &o) != 0 || !a52session_fujinet_running(s))
        return 0;
    for (t = 0; t < 30000 && a52session_fujinet_link(s) != 1; t += 50)
        test_sleep_ms(50);
    CHECK(a52session_fujinet_link(s) == 1, "the cartridge's BoIP link");
    return 1;
}

/* Waits for n more frames (generous: a slow runner). */
static inline int test_wait_frames(a52session *s, int n)
{
    uint64_t start = a52session_frame_count(s);
    int t;
    for (t = 0; t < 60000 && a52session_frame_count(s) < start + (uint64_t)n; t += 10)
        test_sleep_ms(10);
    return a52session_frame_count(s) >= start + (uint64_t)n;
}

static inline int test_peek(a52session *s, unsigned addr)
{
    uint8_t b = 0;
    a52debug_read(a52session_debugger(s), (uint16_t)addr, &b, 1);
    return b;
}

/* Waits up to ms for RAM/ROM at addr to read value. */
static inline int test_wait_byte(a52session *s, unsigned addr, int value, int ms)
{
    int t;
    for (t = 0; t < ms; t += 20) {
        if (test_peek(s, addr) == value)
            return 1;
        test_sleep_ms(20);
    }
    return 0;
}

/* The CONFIG's text screen (mode 2, 40 x 24 at $3C00) as ASCII lines. */
static inline int test_config_text(a52session *s, char *dst, int dstsz)
{
    static uint8_t scr[40 * 24];
    int row, col, k = 0;
    a52debug_read(a52session_debugger(s), 0x3C00, scr, sizeof scr);
    for (row = 0; row < 24 && k + 42 < dstsz; row++) {
        for (col = 0; col < 40; col++) {
            unsigned c = scr[row * 40 + col] & 0x7f;
            c = c < 0x40 ? c + 0x20 : c < 0x60 ? c - 0x40 : c;
            dst[k++] = c >= 0x20 && c < 0x7f ? (char)c : ' ';
        }
        while (k > 0 && dst[k - 1] == ' ')
            k--;
        dst[k++] = '\n';
    }
    dst[k] = 0;
    return k;
}

/* Waits up to ms for text on CONFIG's screen. */
static inline int test_wait_config_text(a52session *s, const char *text, int ms)
{
    static char buf[2048];
    int t;
    for (t = 0; t < ms; t += 50) {
        test_config_text(s, buf, sizeof buf);
        if (strstr(buf, text))
            return 1;
        test_sleep_ms(50);
    }
    fprintf(stderr, "CONFIG's screen was:\n%s\n", buf);
    return 0;
}

/* Waits up to ms for text in FujiNet's console log. */
static inline int test_wait_fujinet_log(a52session *s, const char *text, int ms)
{
    static char buf[1 << 20];
    int t;
    for (t = 0; t < ms; t += 100) {
        a52session_fujinet_copy_log(s, buf, sizeof buf);
        if (strstr(buf, text))
            return 1;
        test_sleep_ms(100);
    }
    return 0;
}

/* The current frame as a PPM, for looking at by hand. */
static inline void test_save_ppm(a52session *s, const char *path)
{
    static uint32_t fb[A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT];
    uint64_t serial = 0;
    int w, h, par, i;
    FILE *fp;
    a52session_copy_frame(s, fb, &w, &h, &par, &serial);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8),
                                (unsigned char)fb[i]};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

static inline void test_set_pref(a52session *s, const char *key, int value)
{
    int i = a52session_pref_find(key);
    CHECK(i >= 0, "no pref %s", key);
    if (a52session_pref(i)->type == A52_PREF_CHOICE)
        a52session_pref_set_choice(s, i, value);
    else
        a52session_set_int(s, key, value);
}

#endif
