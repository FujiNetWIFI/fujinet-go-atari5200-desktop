/* The real in-process FujiNet over BoIP, end to end: the cartridge dials in,
   CONFIG (the baked client) reads the host slots, the user's keypad/stick
   opens the SD host and boots a game from it (MOUNT_IMAGE -> the cartridge
   stages it -> BOOTLOCK -> the BIOS's JMP $B5F0 stub swaps it in) and it runs;
   the debugger's FujiNet trace saw the traffic; a power cycle brings CONFIG
   back; Restart FujiNet is followed by the cartridge's reconnect.
   SKIPs (77) when the runtime was not built. */
#include "test_util.h"
#include "test_files.h"

static void tap(a52session *s, int target)
{
    a52session_press(s, target, 1);
    test_wait_frames(s, 6);
    a52session_press(s, target, 0);
    test_wait_frames(s, 12);
}

/* The debugger's FujiNet trace saw CONFIG's commands and FujiNet's replies. */
static void check_trace(a52session *s)
{
    static a52_fuji_packet p[512];
    int n = a52debug_fujinet_trace(a52session_debugger(s), p, 512), i, req = 0, rep = 0;
    for (i = 0; i < n; i++) {
        if (!p[i].to_host)
            req++;
        else
            rep++;
    }
    CHECK(req > 0 && rep > 0, "FujiNet trace: %d packets, %d to FujiNet, %d replies", n, req, rep);
}

int main(void)
{
    a52session *s = test_session_ex("netboot", 1);
    char path[900], text[160];
    int t;

    CHECK(s, "new");
    if (!test_start_fujinet(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime (%s)\n", a52session_last_error(s));
        a52session_free(s);
        return 77;
    }
    snprintf(path, sizeof path, "%s/game.a52", a52session_sd_path(s));
    CHECK(write_game(path, 0x2000, 0) == 0, "a game on the SD card");

    /* CONFIG: the host list from the live FujiNet */
    CHECK(test_wait_config_text(s, "HOST SLOTS", 60000), "CONFIG draws");
    CHECK(test_wait_config_text(s, "1 SD", 60000), "the SD host is listed");
    check_trace(s);
    a52session_cart_status(s, text, sizeof text);
    CHECK(strstr(text, "connected"), "cart status: %s", text);
    CHECK(a52session_fujinet_link(s) == 1, "link");

    /* open the host, move off the header row to the game, open it */
    CHECK(test_wait_config_text(s, "PLAY GAME LOBBY", 60000), "CONFIG finished drawing");
    test_wait_frames(s, 120);
    for (t = 0; t < 5; t++) {
        /* a press CONFIG was too busy to take is repeated */
        tap(s, A52_TARGET_PORT(0, A52_ACT_FIRE));
        if (test_wait_config_text(s, "game.a52", 15000))
            break;
    }
    CHECK(t < 5, "the directory lists the game");
    test_wait_frames(s, 120);
    tap(s, A52_TARGET_PORT(0, A52_ACT_DOWN));
    test_wait_frames(s, 60);
    for (t = 0; t < 5; t++) {
        /* a press CONFIG was too busy to take is repeated */
        tap(s, A52_TARGET_PORT(0, A52_ACT_FIRE));
        if (test_wait_config_text(s, "LOADING", 10000) || test_wait_byte(s, 0x0400, 0xAA, 1000))
            break;
    }
    if (!test_wait_byte(s, 0x0400, 0xAA, 90000)) {
        static char scr[2048];
        test_config_text(s, scr, sizeof scr);
        fprintf(stderr, "CONFIG's screen:\n%s\n", scr);
    }
    CHECK(test_peek(s, 0x0400) == 0xAA, "the game was pushed, staged, swapped in and runs");
    CHECK(a52session_cart_booted_game(s), "booted a game");
    a52session_cart_info(s, text, sizeof text);
    CHECK(strstr(text, "ROM"), "cart info: %s", text);

    /* power cycle: CONFIG is back */
    a52session_power_cycle(s);
    CHECK(test_wait_config_text(s, "HOST SLOTS", 60000), "CONFIG after the power cycle");
    CHECK(!a52session_cart_booted_game(s), "no game");

    /* Restart FujiNet: the old socket is dead; the cartridge dials again before
       its next transaction (WIFI, the star key, makes one) and CONFIG goes on
       talking to the new instance */
    CHECK(a52session_fujinet_restart(s) == 0, "restart FujiNet");
    CHECK(test_wait_config_text(s, "HOST SLOTS", 60000), "CONFIG still up");
    test_wait_frames(s, 120);
    for (t = 0; t < 6; t++) {
        tap(s, A52_TARGET_KEY(A52_KEY_STAR));
        if (test_wait_fujinet_log(s, "Fuji cmd", 8000))
            break;
    }
    CHECK(t < 6, "the new FujiNet is spoken to");
    CHECK(a52session_fujinet_link(s) == 1, "reconnected");

    a52session_stop(s);
    a52session_free(s);
    puts("netboot: ok");
    return 0;
}
