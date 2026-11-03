/* The 5200's controls: keypad keys and START/PAUSE/RESET reach POKEY (the
   keypad's KBCODE and the IRQ), the stick's digital and analog paths reach the
   POT lines, fire reaches the trigger, bindings steal and persist. No ROM. */
#include "test_util.h"

static int kbcode(a52session *s) { return test_peek(s, 0xE809); }   /* POKEY KBCODE */

int main(void)
{
    a52session *s = test_session("input");
    a52_binding b;
    char stolen[200], name[64];
    int x, y, n, i;
    float w, h;
    const a52_keycap *caps;

    CHECK(s, "session");
    /* defaults: arrows + space, F3-F5, digits */
    b = a52session_binding_get(s, A52_TARGET_PORT(0, A52_ACT_UP));
    CHECK(b.keysym == A52_KEYSYM_UP, "up is the Up arrow");
    CHECK(a52session_target_for_key(s, '5') == A52_TARGET_KEY(A52_KEY_5), "5");
    CHECK(a52session_target_for_key(s, A52_KEYSYM_F5) == A52_TARGET_KEY(A52_KEY_RESET), "F5 is RESET");
    CHECK(a52session_target_for_key(s, A52_KEYSYM_F4) == A52_TARGET_KEY(A52_KEY_START), "F4 is START");
    CHECK(a52session_target_for_key(s, A52_KEYSYM_F3) == A52_TARGET_KEY(A52_KEY_PAUSE), "F3 is PAUSE");
    CHECK(a52session_key_sysaction(s, '5') == -1, "5 is no system action");
    CHECK(a52session_target_for_button(s, 0, A52_PAD_BTN_START) == A52_TARGET_KEY(A52_KEY_START), "pad start");
    CHECK(a52session_target_for_button(s, 2, A52_PAD_BTN_SOUTH) == A52_TARGET_PORT(2, A52_ACT_FIRE), "port 3 fire");

    /* stealing */
    a52session_binding_set_key(s, A52_TARGET_PORT(1, A52_ACT_UP), 'w', stolen, sizeof stolen);
    CHECK(stolen[0] == 0, "w was free");
    a52session_binding_set_key(s, A52_TARGET_PORT(1, A52_ACT_DOWN), 'w', stolen, sizeof stolen);
    CHECK(strstr(stolen, "Controller 2: Up"), "stolen: %s", stolen);
    CHECK(a52session_target_for_key(s, 'W') == A52_TARGET_PORT(1, A52_ACT_DOWN), "folded");
    a52session_bindings_reset(s);
    CHECK(a52session_target_for_key(s, 'w') == -1, "reset");
    a52session_keysym_name(A52_KEYSYM_F4, name, sizeof name);
    CHECK(!strcmp(name, "F4"), "name %s", name);
    CHECK(!strcmp(a52_target_name(A52_TARGET_KEY(A52_KEY_HASH)), "Key #"), "target name");
    CHECK(!strcmp(a52_key_label(A52_KEY_RESET), "RESET"), "label");

    /* the panel */
    caps = a52session_keyboard_layout(&n, &w, &h);
    CHECK(n == A52_KEY_COUNT && w > 0 && h > 0, "layout");
    for (i = 0; i < n; i++)
        CHECK(caps[i].key == i && caps[i].w > 0, "cap %d", i);

    test_start(s, 0);
    CHECK(test_wait_frames(s, 30), "runs");

    /* a keypad key: POKEY's KBCODE changes while it is held, and a tap too short for a
       frame still lands */
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 1);
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 0);
    CHECK(test_wait_frames(s, 6), "frames");
    a52session_press(s, A52_TARGET_KEY(A52_KEY_START), 1);
    CHECK(a52session_keys_held(s) & (1u << A52_KEY_START), "held mask");
    CHECK(test_wait_frames(s, 4), "frames");
    CHECK(kbcode(s) != 0, "KBCODE after START (%02X)", kbcode(s));
    a52session_press(s, A52_TARGET_KEY(A52_KEY_START), 0);
    a52session_release_all(s);
    CHECK(a52session_keys_held(s) == 0, "released");

    /* the stick: digital drives full deflection, the pointer overrides */
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_RIGHT), 1);
    a52session_stick_get(s, 0, &x, &y);
    CHECK(x == 127 && y == 0, "digital right %d,%d", x, y);
    CHECK(a52session_buttons_held(s, 0) == (1u << A52_ACT_RIGHT), "held bits");
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_RIGHT), 0);
    a52session_stick_set(s, 1, -60, 30);
    a52session_stick_get(s, 1, &x, &y);
    CHECK(x == -60 && y == 30, "analog %d,%d", x, y);
    a52session_stick_set(s, 1, 500, -500);
    a52session_stick_get(s, 1, &x, &y);
    CHECK(x == 127 && y == -127, "clamped %d,%d", x, y);
    a52session_stick_set(s, 1, 0, 0);
    CHECK(test_wait_frames(s, 3), "frames");

    /* fire 2 and fire reach the machine without a crash; the console keeps running */
    a52session_press(s, A52_TARGET_PORT(0, A52_ACT_FIRE), 1);
    a52session_press(s, A52_TARGET_PORT(3, A52_ACT_FIRE2), 1);
    CHECK(test_wait_frames(s, 4), "frames");
    a52session_release_all(s);
    a52session_set_analog(s, 0);
    CHECK(a52session_get_int(s, "analog_stick", 1) == 0, "analog setting");

    a52session_stop(s);
    a52session_free(s);
    puts("input: ok");
    return 0;
}
