/* Gamepads come and go: an SDL virtual pad is picked up as controller 1 when
   plugged in, drives the stick, fire and the console keys, is its own input
   source (a key or the window letting go never drops its hold, and it never
   drops theirs), its analog stick and the window's drag are merged strongest
   deflection wins, it is released when unplugged while held, gets its chosen
   port back when plugged in again and on the next run, and with the 2-port BIOS
   cannot be assigned to a port the console does not have. SKIPs (77) where SDL
   has no virtual joysticks or real pads are in the way. */
#include <SDL3/SDL.h>

#include "test_util.h"

#define PAD_NAME "a52test virtual pad"
#define B(act) (1u << (act))
#define WAIT_TRIES 500   /* x 20 ms = 10 s: slow CI runners */

uint32_t roms_crc32(const uint8_t *data, size_t len);
void roms_test_set_crc(int slot, uint32_t crc);

static int failures;

static void expect(a52session *s, int port, unsigned want, unsigned console, const char *what)
{
    unsigned held = 0, con = 0;
    int i;
    for (i = 0; i < WAIT_TRIES; i++) {
        held = a52session_buttons_held(s, port);
        con = a52session_keys_held(s);
        if (held == want && con == console) {
            printf("ok: %s\n", what);
            return;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, console %02X want %02X)\n", what, held, want, con, console);
    failures++;
}

/* Waits for the merged stick of a port to satisfy lo <= x <= hi, y == 0. */
static int wait_stick_x(a52session *s, int port, int lo, int hi, const char *what)
{
    int i, xpos = 0, ypos = 0;
    for (i = 0; i < WAIT_TRIES; i++) {
        a52session_stick_get(s, port, &xpos, &ypos);
        if (xpos >= lo && xpos <= hi && ypos == 0) {
            printf("ok: %s (%d)\n", what, xpos);
            return xpos;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (%d,%d, want x %d..%d)\n", what, xpos, ypos, lo, hi);
    failures++;
    return xpos;
}

static SDL_Joystick *plug(SDL_JoystickID *vid)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    *vid = SDL_AttachVirtualJoystick(&desc);
    return *vid ? SDL_OpenJoystick(*vid) : NULL;
}

static int find_pad(a52session *s)
{
    int i;
    for (i = 0; i < a52session_gamepad_count(s); i++) {
        char name[64];
        a52session_gamepad_name(s, i, name, sizeof name);
        if (!strcmp(name, PAD_NAME))
            return i;
    }
    return -1;
}

int main(void)
{
    a52session *s = test_session("pads");
    a52session_start_opts o;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    char ev[160];
    int i, idx, baseline;
    unsigned gen;

    a52session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    o.sync_cart = 1;
    o.cart_path = "";
    CHECK(a52session_start(s, &o) == 0, "start: %s", a52session_last_error(s));
    test_sleep_ms(700);
    baseline = a52session_gamepad_count(s);
    if (baseline > 0) {
        printf("SKIP: real pads attached\n");
        a52session_free(s);
        return 77;
    }
    gen = a52session_gamepad_generation(s);
    vj = plug(&vid);
    if (!vj) {
        printf("SKIP: no virtual joysticks: %s\n", SDL_GetError());
        a52session_free(s);
        return 77;
    }
    for (i = 0; i < WAIT_TRIES && (idx = find_pad(s)) < 0; i++)
        test_sleep_ms(20);
    CHECK(idx >= 0 && a52session_gamepad_effective_port(s, idx) == 0, "plugged in: joystick 1");
    CHECK(a52session_gamepad_generation(s) != gen, "generation moved");
    a52session_gamepad_last_event(s, ev, sizeof ev);
    CHECK(strstr(ev, "Connected"), "event: %s", ev);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, 0, B(A52_ACT_FIRE), 0, "South fires");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, 0, B(A52_ACT_UP), 0, "D-pad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    {
        int x = 0, y = 0;
        for (i = 0; i < WAIT_TRIES && x <= 0; i++) {
            a52session_stick_get(s, 0, &x, &y);
            test_sleep_ms(20);
        }
        if (x > 0 && y == 0)
            printf("ok: the analog stick right (%d)\n", x);
        else {
            printf("FAIL: the analog stick right (%d,%d)\n", x, y);
            failures++;
        }
    }
    a52session_set_analog(s, 0);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "the stick right, digital");
    a52session_set_analog(s, 1);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect(s, 0, 0, 0, "centred");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, B(A52_KEY_START), "Start is START");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, true);
    expect(s, 0, 0, B(A52_KEY_PAUSE), "Back is PAUSE");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, false);
    expect(s, 0, 0, 0, "released");

    /* every way of holding is a source of its own: the keyboard, the window and the
       pad can only release what they hold themselves */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "the pad holds right");
    a52session_key(s, A52_KEYSYM_RIGHT, 1);
    a52session_key(s, A52_KEYSYM_RIGHT, 0);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "a key letting go leaves the pad's right");
    a52session_key(s, A52_KEYSYM_LEFT, 1);
    expect(s, 0, 0, 0, "the key's left and the pad's right cancel");
    a52session_key(s, A52_KEYSYM_LEFT, 0);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "and the pad's right is back");
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_RIGHT), 1);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "the pad letting go leaves the window's right");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_FIRE), 1);
    expect(s, 0, B(A52_ACT_RIGHT) | B(A52_ACT_FIRE), 0, "fire from the pad and the window");
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_FIRE), 0);
    expect(s, 0, B(A52_ACT_RIGHT) | B(A52_ACT_FIRE), 0, "the window letting go leaves the pad's fire");
    a52session_release_all(s);
    expect(s, 0, B(A52_ACT_RIGHT) | B(A52_ACT_FIRE), 0, "release_all is the keyboard's only");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, B(A52_ACT_RIGHT), 0, "the pad's fire is gone");
    a52session_release_pointer(s);
    expect(s, 0, 0, 0, "release_pointer lets go of the window's right");
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 1);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, B(A52_KEY_5) | B(A52_KEY_START), "a window key and a pad key at once");
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    expect(s, 0, 0, 0, "both let go");

    /* the analog stick: the strongest deflection of any source wins, and the window's
       drag ending at 0,0 never cancels the pad */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    {
        const int padx = wait_stick_x(s, 0, 30, 126, "the pad's stick steers");
        int xpos = 0, ypos = 0;
        a52session_stick_set(s, 0, -120, 0);
        wait_stick_x(s, 0, -120, -120, "a harder drag wins");
        a52session_stick_set(s, 0, 0, 0);
        wait_stick_x(s, 0, padx, padx, "the drag letting go does not cancel the pad");
        a52session_stick_set(s, 0, 10, 0);
        wait_stick_x(s, 0, padx, padx, "a gentler drag loses to the pad");
        a52session_stick_set(s, 0, 0, 0);
        expect(s, 0, 0, 0, "an analog stick presses no direction");
        a52session_stick_get(s, 1, &xpos, &ypos);
        CHECK(xpos == 0 && ypos == 0, "and port 2 is centred");
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    wait_stick_x(s, 0, 0, 0, "the pad centred");

    /* A trigger past half travel used to deadlock the gamepad thread on its
     * own lock, and the Controllers panel's timers (same lock) froze the UI
     * with it. A hang here is a ctest timeout. */
    a52session_binding_set_button(s, A52_TARGET_KEY(A52_KEY_RESET), A52_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, 0, 0, B(A52_KEY_RESET), "left trigger presses its binding");
    (void)a52session_gamepad_generation(s);
    (void)a52session_gamepad_effective_port(s, idx);
    printf("ok: session lock still free with a trigger held\n");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, 0, "left trigger released");
    a52session_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = A52_PAD_BTN_NONE, got = 0;
        for (i = 0; i < WAIT_TRIES && !got; i++) {
            got = a52session_gamepad_capture_poll(s, &btn);
            if (!got) test_sleep_ms(20);
        }
        CHECK(got && btn == A52_PAD_BTN_RIGHT_TRIGGER, "map mode captures a trigger (%d)", btn);
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    a52session_bindings_reset(s);

    /* chosen port 2 (remembered), unplugged while holding a direction, the analog
       stick and a key; the keyboard's own hold on controller 1 stays */
    a52session_gamepad_assign(s, idx, 1);
    CHECK(strstr(a52session_get_str(s, "pad_assign", ""), PAD_NAME) != NULL &&
          strstr(a52session_get_str(s, "pad_assign", ""), "=1") != NULL, "the assignment is saved: %s",
          a52session_get_str(s, "pad_assign", ""));
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 1, B(A52_ACT_RIGHT), 0, "assigned to controller 2");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 1, B(A52_ACT_RIGHT), B(A52_KEY_START), "the pad on port 2 holds right and START");
    a52session_key(s, A52_KEYSYM_SPACE, 1);
    expect(s, 0, B(A52_ACT_FIRE), B(A52_KEY_START), "the keyboard holds controller 1's fire");
    gen = a52session_gamepad_generation(s);
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    expect(s, 1, 0, 0, "unplugged while held: released");
    {
        int xpos = 1, ypos = 1;
        for (i = 0; i < WAIT_TRIES && (xpos || ypos); i++) {
            a52session_stick_get(s, 1, &xpos, &ypos);
            test_sleep_ms(20);
        }
        CHECK(xpos == 0 && ypos == 0, "and so is its analog deflection (%d,%d)", xpos, ypos);
    }
    expect(s, 0, B(A52_ACT_FIRE), 0, "while the keyboard's hold stays");
    a52session_key(s, A52_KEYSYM_SPACE, 0);
    expect(s, 0, 0, 0, "and lets go itself");
    for (i = 0; i < WAIT_TRIES && a52session_gamepad_generation(s) == gen; i++)
        test_sleep_ms(20);
    a52session_gamepad_last_event(s, ev, sizeof ev);
    CHECK(strstr(ev, "Disconnected") && find_pad(s) < 0, "unplug event: %s", ev);

    /* plugged in again: controller 2 again */
    vj = plug(&vid);
    for (i = 0; i < WAIT_TRIES && (idx = find_pad(s)) < 0; i++)
        test_sleep_ms(20);
    CHECK(idx >= 0 && a52session_gamepad_effective_port(s, idx) == 1, "replugged: controller 2 again");
    CHECK(a52session_gamepad_assignment(s, idx) == 1, "as an explicit assignment");

    /* and on the next run, from the saved setting (SDL's virtual joysticks do not
       survive its joystick subsystem restarting: plug it in again after) */
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    a52session_stop(s);
    CHECK(a52session_start(s, &o) == 0, "restart: %s", a52session_last_error(s));
    vj = plug(&vid);
    CHECK(vj != NULL, "plug after restart");
    for (i = 0; i < WAIT_TRIES && (idx = find_pad(s)) < 0; i++)
        test_sleep_ms(20);
    CHECK(idx >= 0 && a52session_gamepad_effective_port(s, idx) == 1, "a new run: controller 2 again (pad %d, port %d, saved '%s')",
          idx, a52session_gamepad_effective_port(s, idx), a52session_get_str(s, "pad_assign", ""));
    a52session_gamepad_last_event(s, ev, sizeof ev);
    CHECK(strstr(ev, "Connected") != NULL, "a pad plugged in after a restart is announced: %s", ev);

    /* Automatic forgets it */
    a52session_gamepad_assign(s, idx, -1);
    CHECK(a52session_gamepad_effective_port(s, idx) == 0 && a52session_gamepad_assignment(s, idx) == -1,
          "Automatic: controller 1");
    CHECK(strstr(a52session_get_str(s, "pad_assign", ""), PAD_NAME) == NULL, "and the setting is cleared");

    /* a port the console does not have is no assignment */
    CHECK(a52session_active_ports(s) == 4, "the built-in BIOS has four ports");
    a52session_gamepad_assign(s, idx, 4);
    CHECK(a52session_gamepad_assignment(s, idx) == -1, "port 5 does not exist");

    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    a52session_stop(s);
    a52session_free(s);

    /* the 2-port BIOS (CRC c2ba2613) has two controllers: the gamepad layer assigns
       no pad past them (a stand-in image with the CRC registered, as rom_test does) */
    {
        static uint8_t image[2048];
        a52session *s2 = test_session("pads2");
        char path[700], msg[256];
        FILE *fp;
        size_t pos;
        for (pos = 0; pos < sizeof image; pos++)
            image[pos] = (uint8_t)((pos * 29 + 5 + (pos >> 4)) & 0xff);
        roms_test_set_crc(1, roms_crc32(image, sizeof image));
        snprintf(path, sizeof path, "%s/bios2.rom", a52session_data_path(s2));
        test_mkdir(a52session_data_path(s2));
        fp = fopen(path, "wb");
        CHECK(fp && fwrite(image, 1, sizeof image, fp) == sizeof image, "write the stand-in BIOS");
        fclose(fp);
        CHECK(a52session_rom_import(s2, path, 1, msg, sizeof msg) == 1, "import it: %s", msg);
        CHECK(a52session_active_ports(s2) == 4, "still four until the 2-port BIOS is chosen");
        a52session_set_int(s2, "bios", 2);
        a52session_default_opts(s2, &o);
        o.enable_fujinet = 0;
        o.enable_audio = 0;
        o.enable_gamepad = 1;
        o.sync_cart = 1;
        o.cart_path = "";
        if (a52session_start(s2, &o) != 0) {
            printf("SKIP: the 2-port part: %s\n", a52session_last_error(s2));
        }
        else {
            vj = plug(&vid);
            for (i = 0; i < WAIT_TRIES && (idx = find_pad(s2)) < 0; i++)
                test_sleep_ms(20);
            CHECK(idx >= 0, "the pad shows up on the 2-port console");
            CHECK(a52session_active_ports(s2) == 2, "the 2-port BIOS has two controllers");
            a52session_gamepad_assign(s2, idx, 2);
            CHECK(a52session_gamepad_assignment(s2, idx) == -1 && a52session_gamepad_effective_port(s2, idx) == 0,
                  "controller 3 does not exist: Automatic");
            a52session_gamepad_assign(s2, idx, 1);
            CHECK(a52session_gamepad_effective_port(s2, idx) == 1, "controller 2 does");
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
            expect(s2, 1, B(A52_ACT_FIRE), 0, "and it plays there");
            SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
            SDL_CloseJoystick(vj);
            SDL_DetachVirtualJoystick(vid);
            a52session_stop(s2);
        }
        a52session_free(s2);
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
