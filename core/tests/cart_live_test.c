/* The FujiNet 5200 cartridge against a LIVE in-process FujiNet (the RS232
   runtime, BoIP on a private loopback port), driven the way CONFIG drives it:
   by READING mailbox hotspots. READ HOST SLOTS (the ACKSEQ echo), then MOUNT_HOST /
   SET_DEVICE_FULLPATH / MOUNT_IMAGE of a generated .a52 in the SD folder, the push
   staging it, the boot state READY, BOOTLOCK, FN_R_ARMED, then the read of $B5F2
   swaps it in. Both the worker-thread and the inline (sync) cartridge. SKIPs (77)
   when the runtime was not built. Seconds-long timeouts only. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "dynlib.h"
#include "fujinet_cart.h"
#include "fuji_crc.h"
#include "fuji_mailbox.h"
#include "fujibus.h"
#include "test_tmpdir.h"

#ifdef _WIN32
#include <windows.h>
#define test_usleep_ms(ms) Sleep(ms)
#define test_setenv(k, v) _putenv_s(k, v)
#else
#include <unistd.h>
#include <time.h>
static void test_usleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#define test_setenv(k, v) setenv(k, v, 1)
#endif

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); test_usleep_ms(300); _exit(1); } } while (0)

#ifndef A52_DEV_FUJINET_OUT
#define A52_DEV_FUJINET_OUT ""
#endif

static a52_fujinet_t *cart;

static uint8_t rd(unsigned a) { return a52_fujinet_read(cart, (uint16_t)a, 0); }
static uint8_t pk(unsigned a) { return a52_fujinet_read(cart, (uint16_t)a, 1); }

/* ---- the client side: the register protocol, by reads */
static void reg(unsigned r, unsigned v) { (void)rd(0xB500 + r); (void)rd(0xB600 + v); }
static void txb(unsigned b) { (void)rd(0xB700 + b); }

static void begin(unsigned dev, unsigned cmd, unsigned nparam)
{
    reg(FN_REG_DEVICE, dev);
    reg(FN_REG_CMD, cmd);
    reg(FN_REG_NPARAM, nparam);
    reg(FN_REG_DATA_RST, 0);
}
static void pbyte(unsigned v) { txb(1); txb(v); }

/* launch and wait for ACKSEQ; returns FN_R_ERR, reply command in *reply */
static int go(int seconds, unsigned *reply)
{
    uint8_t seq = (uint8_t)(pk(0xB000 + FN_R_ACKSEQ) + 1);
    int t;

    if (!seq)
        seq = 1;
    reg(FN_REG_SEQ, seq);
    for (t = 0; t < seconds * 100; t++) {
        if (pk(0xB000 + FN_R_ACKSEQ) == seq) {      /* a read drains the worker's pokes */
            if (reply)
                *reply = pk(0xB000 + FN_R_REPLY_CMD);
            return pk(0xB000 + FN_R_ERR);
        }
        test_usleep_ms(10);
    }
    CHECK(0, "no ACKSEQ for seq %u after %d s", seq, seconds);
    return -1;
}

/* ---- a runtime tree */
static void copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    struct dirent *e;

    test_mkdir(to);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char a[1024], b[1024];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (stat(a, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            copy_tree(a, b);
        } else {
            FILE *in = fopen(a, "rb"), *out = fopen(b, "wb");
            char buf[8192];
            size_t n;

            while (in && out && (n = fread(buf, 1, sizeof buf, in)) > 0)
                fwrite(buf, 1, n, out);
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    closedir(d);
}

typedef bool (*start_fn)(const char *, const char *, const char *, const char *, int);
typedef void (*stop_fn)(void);
typedef const char *(*err_fn)(void);

static int phase(bool sync, const uint8_t *img, uint32_t len, const char *boip)
{
    char why[128];
    a52_cart_status_t st;
    unsigned reply = 0;
    int err, t;
    uint8_t path[256];
    uint32_t crc = fuji_crc32(0, img, len);

    CHECK(a52_fujinet_power_on(cart, NULL, 0, boip, sync, why, sizeof why) == 0, "power_on %s", why);
    CHECK(pk(0xB409) == 'F' && pk(0xB40A) == 'N', "CONFIG's mailbox painted");

    /* READ HOST SLOTS */
    begin(0x70, 0xF4, 0);
    err = go(30, &reply);
    CHECK(err == FN_ERR_OK && reply == 0x06, "READ HOST SLOTS err %d reply %02X", err, reply);
    CHECK((pk(0xB000 + FN_R_RXLEN_LO) | pk(0xB000 + FN_R_RXLEN_HI) << 8) > 0, "reply has data");
    CHECK(pk(0xB000 + FN_R_STATUS) & FN_R_STATUS_LINK, "link up");
    a52_fujinet_status(cart, &st);
    CHECK(st.link_up && !st.link_off, "status link");

    /* MOUNT_HOST(0) */
    begin(0x70, 0xF9, 1);
    pbyte(0);
    err = go(30, &reply);
    CHECK(err == FN_ERR_OK && reply == 0x06, "MOUNT_HOST err %d reply %02X", err, reply);

    /* SET_DEVICE_FULLPATH(dev 0, host 0, read, "/game.a52") */
    memset(path, 0, sizeof path);
    memcpy(path, "/game.a52", 9);
    begin(0x70, 0xE2, 3);
    pbyte(0); pbyte(0); pbyte(1);
    for (t = 0; t < 256; t++)
        txb(path[t]);
    err = go(30, &reply);
    CHECK(err == FN_ERR_OK && reply == 0x06, "SET_DEVICE_FULLPATH err %d reply %02X", err, reply);

    /* MOUNT_IMAGE(dev 0, read): the server pushes the image to us */
    begin(0x70, 0xF8, 2);
    pbyte(0); pbyte(1);
    err = go(90, &reply);
    CHECK(err == FN_ERR_OK && reply == 0x06, "MOUNT_IMAGE err %d reply %02X", err, reply);
    for (t = 0; t < 600 && pk(0xB000 + FN_R_BOOT_STATE) == FN_BOOT_XFER; t++)
        test_usleep_ms(10);
    CHECK(pk(0xB000 + FN_R_BOOT_STATE) == FN_BOOT_READY, "boot state %02X err %02X",
          pk(0xB000 + FN_R_BOOT_STATE), pk(0xB000 + FN_R_BOOT_ERR));
    CHECK(pk(0xB000 + FN_R_STAGED) & FN_STAGED_READY, "staged ready");
    CHECK(pk(0xB000 + FN_R_STAGED_KIND) == 0, "staged mapper ROM");
    CHECK(pk(0xB000 + FN_R_STAGED_CRC) == (crc & 0xFF) && pk(0xB000 + FN_R_STAGED_CRC + 3) == (crc >> 24),
          "staged crc");
    CHECK(pk(0x4000) != img[0] || img[0] == pk(0x4000), "(still CONFIG)");
    a52_fujinet_status(cart, &st);
    CHECK(st.staged && st.staged_crc == crc && st.staged_size == len && st.swaps == 0 && !st.booted_game,
          "status staged: %d crc %08X size %u", st.staged, st.staged_crc, st.staged_size);

    /* BOOTLOCK: arm, poll FN_R_ARMED, then the stub's last byte swaps */
    reg(FN_REG_BOOTLOCK, FN_BOOTLOCK_MAGIC);
    for (t = 0; t < 600 && !pk(0xB000 + FN_R_ARMED); t++)
        test_usleep_ms(10);
    CHECK(pk(0xB000 + FN_R_ARMED) == 1, "armed");
    {
        unsigned s = a52_fujinet_view_serial(cart);
        (void)rd(0xB5F2);
        CHECK(a52_fujinet_view_serial(cart) != s, "serial moved on the swap");
    }
    a52_fujinet_status(cart, &st);
    CHECK(st.swaps == 1 && st.live_crc == crc && st.booted_game && !st.armed && !st.staged,
          "swapped: swaps %u crc %08X", st.swaps, st.live_crc);
    for (t = 0; t < 0x8000; t += 0x61)
        CHECK(pk(0x4000 + t) == img[t], "swapped view at $%04X", 0x4000 + t);
    CHECK(!st.mailbox, "a plain game: mailbox off");
    return 0;
}

int main(void)
{
    const char *out = getenv("A52_TEST_FUJINET_OUT");
    char lib[1100], base[512], root[600], sd[700], data[700], cfg[700], p[800], boip[64], web[64];
    unsigned port = 20000 + (unsigned)(test_getpid() % 20000);
    a52_dynlib h;
    start_fn start;
    stop_fn stop;
    err_fn lasterr;
    uint8_t img[0x8000];
    FILE *fp;

    if (!out || !*out)
        out = A52_DEV_FUJINET_OUT;
#ifdef _WIN32
    snprintf(lib, sizeof lib, "%s/fujinet.dll", out);
#elif defined(__APPLE__)
    snprintf(lib, sizeof lib, "%s/libfujinet.dylib", out);
#else
    snprintf(lib, sizeof lib, "%s/libfujinet.so", out);
#endif
    if (!*out || (fp = fopen(lib, "rb")) == NULL) {
        fprintf(stderr, "SKIP: no FujiNet runtime at %s\n", lib);
        return 77;
    }
    fclose(fp);

    test_tmpdir(base, sizeof base, "cartlive");
    snprintf(root, sizeof root, "%s/fujinet", base);
    snprintf(sd, sizeof sd, "%s/SD", root);
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(cfg, sizeof cfg, "%s/fnconfig.ini", root);
    test_mkdir(root);
    test_mkdir(sd);
    snprintf(p, sizeof p, "%s/data", out);
    copy_tree(p, data);
    fp = fopen(cfg, "w");
    CHECK(fp, "write fnconfig");
    fprintf(fp, "[General]\ndevicename=fujinet-cart-test\nconfig_enabled=1\nboot_mode=0\n"
                "[Host1]\ntype=SD\nname=SD\n[BOIP]\nenabled=1\nhost=127.0.0.1\nport=%u\n", port);
    fclose(fp);

    for (unsigned i = 0; i < sizeof img; i++)
        img[i] = (uint8_t)(i * 131u + (i >> 8) * 17u + 77u);
    snprintf(p, sizeof p, "%s/game.a52", sd);
    fp = fopen(p, "wb");
    CHECK(fp, "write game");
    fwrite(img, 1, sizeof img, fp);
    fclose(fp);

    h = a52_dynlib_open(lib);
    if (!h) {
        char eb[256];
        fprintf(stderr, "SKIP: cannot load %s: %s\n", lib, a52_dynlib_error(eb, sizeof eb));
        return 77;
    }
    start = (start_fn)a52_dynlib_sym(h, "fujinet_desktop_start_runtime");
    stop = (stop_fn)a52_dynlib_sym(h, "fujinet_desktop_stop_runtime");
    lasterr = (err_fn)a52_dynlib_sym(h, "fujinet_desktop_last_error_message");
    CHECK(start && stop && lasterr, "runtime entry points");
    snprintf(web, sizeof web, "127.0.0.1:%u", port + 1);
    test_setenv("FUJINET_WEBUI_BIND", web);
    test_setenv("FUJINET_RUNTIME_ROOT", root);
    CHECK(start(root, cfg, sd, data, (int)port), "FujiNet start: %s", lasterr ? lasterr() : "?");

    cart = a52_fujinet_create();
    CHECK(cart, "cart");
    snprintf(boip, sizeof boip, "127.0.0.1:%u", port);
    for (int sync = 0; sync < 2; sync++) {
        fprintf(stderr, "cart_live: %s mode\n", sync ? "sync" : "worker-thread");
        phase(sync != 0, img, sizeof img, boip);
    }
    a52_fujinet_power_off(cart);
    a52_fujinet_destroy(cart);
    stop();
    puts("cart_live: ok");
    return 0;
}
