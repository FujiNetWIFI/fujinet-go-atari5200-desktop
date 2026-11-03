/* The FujiNet 5200 cartridge on its own (no network, no emulator): the image
   planner over generated images of every kind, what the view serves, the
   mapper bank switching by real hotspot reads (expectations from atari800's
   own cartridge.c semantics and MAME's file orders, not from a52_commit), the
   view_serial / copy_window contract, CONFIG at power-on, the platform
   register that takes a running app back to CONFIG, and writes having a
   read's side effects. */
#include <stdbool.h>
#include <stdint.h>

#include "fujinet_cart.h"
#include "fuji_crc.h"
#include "fuji_mailbox.h"
#include "a52map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); _exit(1); } } while (0)

static a52_fujinet_t *cart;

static void pattern(uint8_t *b, uint32_t n, unsigned seed)
{
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((i * 131u + (i >> 8) * 17u + seed * 29u + 5u) ^ (i >> 11));
    /* never accidentally the claim */
}

static uint8_t rd(unsigned a) { return a52_fujinet_read(cart, (uint16_t)a, 0); }
static uint8_t pk(unsigned a) { return a52_fujinet_read(cart, (uint16_t)a, 1); }

static void power(const uint8_t *img, uint32_t len)
{
    char why[128] = "";
    int rc = a52_fujinet_power_on(cart, img, len, "", true, why, sizeof why);
    CHECK(rc == 0, "power_on: %s", why);
}

/* copy_window agrees with every byte the CPU would read, side-effect free */
static void check_window(const char *what)
{
    static uint8_t win[0x8000];
    a52_fujinet_copy_window(cart, win);
    for (unsigned a = 0; a < 0x8000; a += 0x101)
        CHECK(win[a] == pk(0x4000 + a), "%s: window $%04X: %02X vs %02X", what, 0x4000 + a,
              win[a], pk(0x4000 + a));
    for (unsigned p = 0; p < 16; p++)
        CHECK(win[p * 0x800] == pk(0x4000 + p * 0x800) && win[p * 0x800 + 0x7FF] == pk(0x47FF + p * 0x800),
              "%s: page %u edges", what, p);
}

static void check_plan(const char *what, const uint8_t *img, uint32_t len, int ok, int kind,
                       int claim)
{
    bool claims = false;
    int k = -1;
    uint32_t crc = 0;
    char why[128] = "";
    int r = a52_fujinet_check_image(img, len, &claims, &k, &crc, why, sizeof why);
    CHECK(r == ok, "%s: check_image=%d (%s)", what, r, why);
    if (!ok) {
        CHECK(why[0], "%s: no reason", what);
        return;
    }
    if (kind >= 0)
        CHECK(k == kind, "%s: kind %d, wanted %d", what, k, kind);
    CHECK((int)claims == claim, "%s: claim %d", what, claims);
}

static void car(uint8_t *dst, uint32_t type, const uint8_t *img, uint32_t len)
{
    memset(dst, 0, 16);
    memcpy(dst, "CART", 4);
    dst[4] = (uint8_t)(type >> 24); dst[5] = (uint8_t)(type >> 16);
    dst[6] = (uint8_t)(type >> 8);  dst[7] = (uint8_t)type;
    memcpy(dst + 16, img, len);
}

static uint8_t big[0x20000 + 0x1000 + 16], big2[0x20000 + 0x1000 + 16];

int main(void)
{
    a52_cart_status_t st;
    uint8_t arena[A52_FJ_ARENA_BYTES];
    uint32_t cfglen = 0;
    const uint8_t *cfg = a52_fujinet_config_image(&cfglen);
    int k;

    cart = a52_fujinet_create();
    CHECK(cart, "create");
    CHECK(a52_fujinet_create() == NULL, "one cart per process");

    /* ---- CONFIG at power-on */
    CHECK(cfglen == 0x8000 && cfg, "config image");
    power(NULL, 0);
    a52_fujinet_status(cart, &st);
    CHECK(st.powered && st.mailbox && st.claim && !st.booted_game && st.link_off && !st.link_up,
          "CONFIG status");
    CHECK(st.live_size == cfglen && st.live_crc == fuji_crc32(0, cfg, cfglen), "CONFIG crc");
    CHECK(pk(0xB409) == 'F' && pk(0xB40A) == 'N', "magic at $B409: %02X %02X", pk(0xB409), pk(0xB40A));
    CHECK(pk(0xB5F0) == 0x6C && pk(0xB5F1) == 0xFC && pk(0xB5F2) == 0xFF, "stub");
    CHECK(pk(0x4000) == cfg[0] && pk(0x7FFF) == cfg[0x3FFF] && pk(0xA000) == cfg[0x6000]
          && pk(0xAFFF) == cfg[0x6FFF], "CONFIG bytes");
    CHECK(pk(0xBFE0) == 'F' && pk(0xBFE1) == 'U' && pk(0xBFE2) == 'J' && pk(0xBFE3) == 'I', "claim");
    a52_fujinet_arena(cart, arena);
    CHECK(arena[0x409] == 'F' && arena[0x40A] == 'N', "arena copy");
    check_window("config");
    /* outside the window nothing answers; the debugger side is inert */
    CHECK(a52_fujinet_read(cart, 0x3FFF, 0) == 0xFF && a52_fujinet_read(cart, 0xC000, 0) == 0xFF, "open bus");

    /* ---- planner: sizes and kinds */
    pattern(big, 0x20000, 1);
    check_plan("empty", big, 0, 0, -1, 0);
    check_plan("4K", big, 0x1000, 1, A52MAP_ROM, 0);
    check_plan("8K", big, 0x2000, 1, A52MAP_ROM, 0);
    check_plan("16K", big, 0x4000, 1, -1, 0);
    k = -1; a52_fujinet_check_image(big, 0x4000, NULL, &k, NULL, NULL, 0);
    CHECK(k == A52MAP_ROM || k == A52MAP_2CHIPS, "16K kind %d", k);
    check_plan("32K", big, 0x8000, 1, A52MAP_ROM, 0);
    check_plan("40K (Bounty Bob)", big, 0xA000, 1, A52MAP_BBSB, 0);
    check_plan("64K (Super Cart)", big, 0x10000, 1, A52MAP_SUPERCART, 0);
    check_plan("128K (Super Cart)", big, 0x20000, 1, A52MAP_SUPERCART, 0);
    check_plan("96K (not a power of two)", big, 0x18000, 0, -1, 0);
    check_plan("oversized", big, 0x20000 + 0x1000, 0, -1, 0);
    check_plan("NULL", NULL, 100, 0, -1, 0);

    /* the .car header decides */
    {
        uint8_t *c = big2;
        pattern(big, 0x8000, 2);
        car(c, 4, big, 0x8000);
        check_plan(".car 32K", c, 0x8010, 1, A52MAP_ROM, 0);
        pattern(big, 0x4000, 3);
        car(c, 6, big, 0x4000);
        check_plan(".car 2 chips", c, 0x4010, 1, A52MAP_2CHIPS, 0);
        pattern(big, 0xA000, 4);
        car(c, 7, big, 0xA000);
        check_plan(".car type 7", c, 0xA010, 1, A52MAP_BBSB, 0);
        car(c, 4242, big, 0x8000);
        check_plan(".car unknown type", c, 0x8010, 0, -1, 0);
    }

    /* the FUJI claim: a 32K ROM carrying it */
    {
        uint8_t app[0x8000];
        pattern(app, sizeof app, 5);
        memcpy(app + 0x7FE0, "FUJI", 4);
        app[0x7FE5] = 0;                 /* kind: auto */
        check_plan("claim", app, sizeof app, 1, A52MAP_ROM, 1);
    }

    /* ---- ROM mirrors, as rom[a & (size-1)] */
    {
        uint8_t img[0x2000];
        pattern(img, sizeof img, 6);
        power(img, sizeof img);
        a52_fujinet_status(cart, &st);
        CHECK(st.booted_game && !st.mailbox && !st.claim && st.mapper == A52MAP_ROM && st.swaps == 1,
              "8K game status: booted %d mailbox %d swaps %u", st.booted_game, st.mailbox, st.swaps);
        CHECK(st.live_crc == fuji_crc32(0, img, sizeof img) && st.live_size == sizeof img, "8K crc");
        for (unsigned a = 0; a < 0x8000; a += 0x37)
            CHECK(pk(0x4000 + a) == img[a & 0x1FFF], "8K mirror at $%04X", 0x4000 + a);
        check_window("8K");
        /* a plain game: the mailbox pages are ROM, reads change nothing */
        {
            unsigned before = a52_fujinet_view_serial(cart);
            (void)rd(0xB500); (void)rd(0xB6C5); (void)rd(0xB5F2);
            a52_fujinet_status(cart, &st);
            CHECK(st.swaps == 1 && a52_fujinet_view_serial(cart) == before && st.queue == 0,
                  "mailbox must be dead for a plain game");
        }
    }
    {
        uint8_t img[0x8000];
        pattern(img, sizeof img, 7);
        power(img, sizeof img);
        for (unsigned a = 0; a < 0x8000; a += 0x41)
            CHECK(pk(0x4000 + a) == img[a], "32K at $%04X", 0x4000 + a);
        a52_fujinet_view_poke(cart, 0x10, 0x5A);   /* the view's own buffer */
        CHECK(pk(0x4010) == 0x5A, "view_poke");
    }

    /* ---- 2 chips: each 8K twice (file bytes 0-8K at $4000, 8-16K at $8000) */
    {
        uint8_t img[0x4000], c[0x4010];
        pattern(img, sizeof img, 8);
        car(c, 6, img, sizeof img);
        power(c, sizeof c);
        a52_fujinet_status(cart, &st);
        CHECK(st.mapper == A52MAP_2CHIPS, "2chips mapper %d", st.mapper);
        for (unsigned a = 0; a < 0x2000; a += 0x31) {
            CHECK(pk(0x4000 + a) == img[a] && pk(0x6000 + a) == img[a], "2chips low half");
            CHECK(pk(0x8000 + a) == img[0x2000 + a] && pk(0xA000 + a) == img[0x2000 + a],
                  "2chips high half at %04X", a);
        }
    }

    /* ---- Bounty Bob: MAME file order, then atari800 .car type 7 order */
    for (int car7 = 0; car7 < 2; car7++) {
        uint8_t img[0xA000], c[0xA010];
        uint32_t fix, w0, w1;
        pattern(img, sizeof img, 9 + car7);
        /* never a byte equal to the hotspot lie: those read $FF */
        if (car7) {
            car(c, 7, img, sizeof img);
            power(c, sizeof c);
            fix = 0x8000; w0 = 0x0000; w1 = 0x4000;
        } else {
            power(img, sizeof img);
            fix = 0x0000; w0 = 0x2000; w1 = 0x6000;
        }
        a52_fujinet_status(cart, &st);
        CHECK(st.mapper == A52MAP_BBSB && st.bank[0] == 0 && st.bank[1] == 0, "bbsb power-on");
        for (unsigned x = 0; x < 0x1000; x += 0x23) {
            if (x >= 0xFF6 - 0x23)
                continue;
            CHECK(pk(0x4000 + x) == img[w0 + x] && pk(0x5000 + x) == img[w1 + x], "bbsb bank 0 at %03X", x);
        }
        for (unsigned x = 0; x < 0x2000; x += 0x29)
            CHECK(pk(0x8000 + x) == img[fix + x] && pk(0xA000 + x) == img[fix + x], "bbsb fixed 8K");
        check_window("bbsb");
        for (unsigned b = 1; b < 4; b++) {
            unsigned s = a52_fujinet_view_serial(cart);
            CHECK(rd(0x4FF6 + b) == 0xFF, "hotspot answers $FF");
            CHECK(a52_fujinet_view_serial(cart) != s, "bank change moves the serial");
            a52_fujinet_status(cart, &st);
            CHECK(st.bank[0] == (int)b, "$4FF%X selects bank %u (got %d)", 6 + b, b, st.bank[0]);
            for (unsigned x = 0; x < 0xF00; x += 0x1F)
                CHECK(pk(0x4000 + x) == img[w0 + b * 0x1000 + x], "bank %u at %03X", b, x);
            check_window("bbsb bank");
            s = a52_fujinet_view_serial(cart);
            (void)pk(0x5FF6 + (3 - b));       /* no side effects when peeking */
            CHECK(a52_fujinet_view_serial(cart) == s, "peek is inert");
            a52_fujinet_write(cart, (uint16_t)(0x5FF6 + (3 - b)), 0);   /* a write is a read */
            a52_fujinet_status(cart, &st);
            CHECK(st.bank[1] == (int)(3 - b), "write at $5FF%X switches", 6 + 3 - b);
            for (unsigned x = 0; x < 0xF00; x += 0x1F)
                CHECK(pk(0x5000 + x) == img[w1 + (3 - b) * 0x1000 + x], "$5000 bank at %03X", x);
        }
    }

    /* ---- Super Cart: atari800's access_5200SuperCart on the same reads */
    for (int banks = 2; banks <= 4; banks += 2) {
        uint32_t len = (uint32_t)banks * 0x8000;
        int mask = banks - 1, state;
        static const unsigned seq[] = { 0xBFE0, 0xBFD0, 0xBFD4, 0xBFD8, 0xBFDC, 0xBFC0, 0xBFC4,
                                        0xBFC8, 0xBFCC, 0xBFF0, 0xBFCC, 0xBFD8, 0xBFD4, 0xBFD0 };
        pattern(big, len, 11 + banks);
        power(big, len);
        a52_fujinet_status(cart, &st);
        CHECK(st.mapper == A52MAP_SUPERCART && st.nbanks == banks, "supercart %dK status", banks * 32);
        state = st.bank[0];
        for (unsigned x = 0; x < 0x7FC0; x += 0x1D1)
            CHECK(pk(0x4000 + x) == big[(unsigned)state * 0x8000 + x], "power-on bank %d", state);
        for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
            unsigned a = seq[i];
            int ns = state;
            uint8_t got;

            switch (a & 0x30) {
            case 0x00: ns = (state & 3) | (a & 0xC); break;
            case 0x10: ns = (state & 0xC) | ((a & 0xC) >> 2); break;
            default:   ns = 0xF; break;
            }
            ns &= mask;
            got = rd(a);
            CHECK(got == big[(unsigned)ns * 0x8000 + (a - 0x4000)],
                  "supercart %dK read $%04X -> bank %d: byte %02X", banks * 32, a, ns, got);
            a52_fujinet_status(cart, &st);
            CHECK(st.bank[0] == ns, "supercart %dK after $%04X: bank %d, wanted %d", banks * 32, a,
                  st.bank[0], ns);
            state = ns;
            for (unsigned x = 0; x < 0x7FC0; x += 0x3E3)
                CHECK(pk(0x4000 + x) == big[(unsigned)ns * 0x8000 + x], "bank %d at %04X", ns, x);
            check_window("supercart");
        }
        a52_fujinet_write(cart, 0xBFE0, 0);
        a52_fujinet_status(cart, &st);
        CHECK(st.bank[0] == mask, "write at $BFE0 selects the top bank");
    }

    /* ---- an app with the claim boots as the client, and CONFIG comes back by its register */
    {
        uint8_t app[0x8000];
        uint32_t cfgcrc = fuji_crc32(0, cfg, cfglen), appcrc;
        pattern(app, sizeof app, 12);
        memcpy(app + 0x7FE0, "FUJI", 4);
        app[0x7FE5] = 0;
        appcrc = fuji_crc32(0, app, sizeof app);
        power(app, sizeof app);
        a52_fujinet_status(cart, &st);
        CHECK(st.mailbox && st.claim && st.booted_game && st.live_crc == appcrc && st.swaps == 0,
              "app status mb %d claim %d booted %d swaps %u", st.mailbox, st.claim, st.booted_game, st.swaps);
        CHECK(pk(0xB409) == 'F' && pk(0x4000) == app[0], "app serves itself with the mailbox painted");
        /* REGSEL $14 (CONFIG), REGDATA $C5: stage CONFIG and arm */
        (void)rd(0xB500 + FN_REG_CONFIG);
        (void)rd(0xB600 + FN_CONFIG_MAGIC);
        a52_fujinet_status(cart, &st);
        CHECK(st.staged && st.armed && st.staged_config && st.staged_claim, "CONFIG staged and armed");
        CHECK(pk(0x4000) == app[0], "the staged image is not served yet");
        {
            unsigned s = a52_fujinet_view_serial(cart);
            (void)rd(0xB5F2);              /* the stub's last byte: swap */
            CHECK(a52_fujinet_view_serial(cart) != s, "swap moves the serial");
        }
        a52_fujinet_status(cart, &st);
        CHECK(st.swaps == 1 && st.live_crc == cfgcrc && st.mailbox && !st.armed && !st.staged,
              "swapped to CONFIG: swaps %u mailbox %d crc %08X vs %08X armed %d staged %d", st.swaps, st.mailbox, st.live_crc, cfgcrc, st.armed, st.staged);
        CHECK(pk(0x4000) == cfg[0] && pk(0xBFE0) == 'F', "CONFIG served after the swap");
        check_window("after swap");
        /* an unarmed read of the swap byte changes nothing */
        {
            unsigned s = a52_fujinet_view_serial(cart);
            (void)rd(0xB5F2);
            CHECK(a52_fujinet_view_serial(cart) == s, "unarmed swap read is inert");
        }
    }

    /* ---- power off / on again, and an unplannable image refuses cleanly */
    {
        char why[128];
        a52_fujinet_power_off(cart);
        CHECK(!a52_fujinet_powered(cart) && rd(0x4000) == 0xFF, "powered off");
        CHECK(a52_fujinet_power_on(cart, big, 0x18000, "", true, why, sizeof why) == -1 && why[0],
              "refused: %s", why);
        power(NULL, 0);
        a52_fujinet_status(cart, &st);
        CHECK(st.powered && st.swaps == 0 && !st.booted_game, "power cycle is CONFIG again");
    }

    a52_fujinet_destroy(cart);
    puts("cart: ok");
    return 0;
}
