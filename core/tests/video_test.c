/* The picture paths: the three visible areas, NTSC and PAL geometry, and that
   CONFIG's picture is really there (not black). No ROM, no network. */
#include "test_util.h"
#include <stdint.h>

static uint32_t *fb;

static void frame(a52session *s, int *w, int *h, int *par)
{
    uint64_t serial = 0;
    test_wait_frames(s, 8);
    a52session_copy_frame(s, fb, w, h, par, &serial);
}

static int distinct(const uint32_t *p, int n)
{
    uint32_t seen[8];
    int k = 0, i, j;
    for (i = 0; i < n && k < 8; i++) {
        for (j = 0; j < k && seen[j] != p[i]; j++)
            ;
        if (j == k)
            seen[k++] = p[i];
    }
    return k;
}

int main(void)
{
    a52session *s = test_session("video");
    int w, h, par;
    fb = malloc(A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT * 4);
    CHECK(s, "new");
    test_start(s, 0);
    CHECK(test_wait_config_text(s, "HOST SLOTS", 60000), "boot");

    frame(s, &w, &h, &par);
    CHECK(w == 336 && h == 224 && par == 1000, "NTSC TV area %dx%d", w, h);
    CHECK(distinct(fb, w * h) >= 2, "CONFIG's picture has more than one colour");
    test_set_pref(s, "area", 0);
    a52session_apply_settings(s);
    frame(s, &w, &h, &par);
    CHECK(w == 320 && h == 200 && par == 1000, "narrow %dx%d", w, h);
    test_set_pref(s, "area", 2);
    a52session_apply_settings(s);
    frame(s, &w, &h, &par);
    CHECK(w == 384 && h == 240, "overscan %dx%d", w, h);
    test_set_pref(s, "area", 1);
    test_set_pref(s, "tv", 1);
    CHECK(a52session_apply_settings(s) == A52_APPLY_POWER, "PAL needs a power cycle");
    CHECK(test_wait_config_text(s, "HOST SLOTS", 60000), "PAL boot");
    frame(s, &w, &h, &par);
    CHECK(w == 336 && h == 240 && par == 1000, "PAL %dx%d", w, h);
    CHECK(a52session_refresh_rate(s) / 1000 == 49, "PAL rate");
    test_set_pref(s, "tv", 0);
    a52session_apply_settings(s);
    a52session_stop(s);
    a52session_free(s);
    free(fb);
    puts("video: ok");
    return 0;
}
