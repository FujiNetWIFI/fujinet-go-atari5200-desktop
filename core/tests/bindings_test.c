/*
 * bindings_test -- the key/pad bindings table: the defaults, rebinding and the
 * steal it reports, the reserved keys (F9, F11, F12), Clear, persistence of
 * only what differs (and a reserved key in an old settings file is dropped),
 * and that a held key lets go of exactly what its press asserted even when it
 * is rebound while held. No machine is started: the merge needs none.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a52session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static a52session *new_session(const char *tag, char *base, size_t basesz)
{
    char cfg[600], data[600];
    a52session_paths p;
    test_tmpdir(base, basesz, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";
    return a52session_new(&p);
}

#define B(a) (1u << (a))

int main(void)
{
    char base[512], stolen[256], name[64];
    a52session *s = new_session("bind", base, sizeof base);
    a52_binding b;
    const int up1 = A52_TARGET_PORT(0, A52_ACT_UP), fire1 = A52_TARGET_PORT(0, A52_ACT_FIRE);
    const int fire_two = A52_TARGET_PORT(1, A52_ACT_FIRE), start = A52_TARGET_KEY(A52_KEY_START);

    /* defaults */
    check(a52session_target_for_key(s, A52_KEYSYM_UP) == up1, "Up arrow is controller 1 up");
    check(a52session_target_for_key(s, A52_KEYSYM_F4) == start, "F4 is START");
    check(a52session_target_for_key(s, 'Z') == -1, "an unbound key drives nothing");
    check(a52session_binding_get(s, fire1).button == A52_PAD_BTN_SOUTH, "South fires controller 1");
    check(a52session_target_for_key(s, A52_KEYSYM_LSHIFT) == A52_TARGET_PORT(0, A52_ACT_FIRE2), "Left Shift is fire 2");
    check(A52_TARGET_COUNT == 42, "42 targets in all");

    /* reserved keys */
    check(a52session_key_reserved(A52_KEYSYM_F9) && a52session_key_reserved(A52_KEYSYM_F11)
          && a52session_key_reserved(A52_KEYSYM_F12), "F9, F11 and F12 are reserved");
    check(!a52session_key_reserved(A52_KEYSYM_F10) && !a52session_key_reserved('a') && !a52session_key_reserved(A52_KEYSYM_F4),
          "F10, letters and F4 are not");
    strcpy(stolen, "junk");
    a52session_binding_set_key(s, fire_two, A52_KEYSYM_F9, stolen, sizeof stolen);
    check(a52session_binding_get(s, fire_two).keysym == 0 && stolen[0] == '\0', "a reserved key cannot be bound (and steals nothing)");
    a52session_binding_set_key(s, fire_two, A52_KEYSYM_F12, NULL, 0);
    check(a52session_binding_get(s, fire_two).keysym == 0, "not F12 either");

    /* rebinding steals, and says from whom */
    a52session_binding_set_key(s, fire_two, A52_KEYSYM_F4, stolen, sizeof stolen);
    check(strstr(stolen, "START") != NULL, "rebinding F4 reports it took it from START");
    check(a52session_binding_get(s, start).keysym == 0 && a52session_target_for_key(s, A52_KEYSYM_F4) == fire_two, "START lost F4");
    a52session_binding_set_key(s, fire_two, 'Q', NULL, 0);
    check(a52session_target_for_key(s, 'q') == fire_two, "keys are case-folded");

    /* Clear: key AND button */
    a52session_binding_clear(s, fire1);
    b = a52session_binding_get(s, fire1);
    check(b.keysym == 0 && b.button == A52_PAD_BTN_NONE, "Clear takes both the key and the pad button");
    check(a52session_target_for_key(s, A52_KEYSYM_SPACE) == -1, "Space is free");
    a52session_binding_clear(s, -1);
    a52session_binding_clear(s, A52_TARGET_COUNT);
    check(1, "Clear ignores a target out of range");

    /* the macOS window's old "show_bindings" becomes controllers_show_bindings */
    check(a52session_get_str(s, "controllers_show_bindings", NULL) == NULL, "no show-bindings setting yet");
    a52session_set_str(s, "show_bindings", "1");

    /* persistence: only the difference, and it comes back */
    a52session_settings_flush(s);
    a52session_free(s);
    {
        a52session_paths p;
        char cfg[600], data[600];
        snprintf(cfg, sizeof cfg, "%s/config", base);
        snprintf(data, sizeof data, "%s/data", base);
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = a52session_new(&p);
    }
    check(a52session_get_int(s, "controllers_show_bindings", 0) == 1, "show_bindings migrated");
    check(a52session_target_for_key(s, 'q') == fire_two, "a rebinding survives a restart");
    b = a52session_binding_get(s, fire1);
    check(b.keysym == 0 && b.button == A52_PAD_BTN_NONE, "and so does a Clear");
    check(a52session_target_for_key(s, A52_KEYSYM_UP) == up1, "an untouched default is still there");

    /* a reserved key in an old settings file is dropped on load */
    /* F11 (65480) on joystick 1's fire (target 4), F10 (65479) on START (target 36) */
    a52session_set_str(s, "bindings", "4.k:65480 36.k:65479");
    a52session_settings_flush(s);
    a52session_free(s);
    {
        a52session_paths p;
        char cfg[600], data[600];
        snprintf(cfg, sizeof cfg, "%s/config", base);
        snprintf(data, sizeof data, "%s/data", base);
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = a52session_new(&p);
    }
    check(a52session_binding_get(s, A52_TARGET_PORT(0, A52_ACT_FIRE)).keysym == 0, "F11 in an old settings file is dropped");
    check(a52session_binding_get(s, start).keysym == A52_KEYSYM_F10, "while F10 is fine");
    a52session_bindings_reset(s);

    /* a held key lets go of what it pressed, even rebound meanwhile */
    check(a52session_key(s, A52_KEYSYM_UP, 1) == 1, "Up arrow down is taken");
    check(a52session_buttons_held(s, 0) == B(A52_ACT_UP), "and holds controller 1 up");
    check(a52session_key(s, A52_KEYSYM_UP, 1) == 1 && a52session_buttons_held(s, 0) == B(A52_ACT_UP), "auto-repeat changes nothing");
    a52session_binding_set_key(s, A52_TARGET_PORT(1, A52_ACT_UP), A52_KEYSYM_UP, NULL, 0);   /* the Up arrow is now controller 2 up */
    check(a52session_key(s, A52_KEYSYM_UP, 0) == 1, "Up arrow up is taken");
    check(a52session_buttons_held(s, 0) == 0 && a52session_buttons_held(s, 1) == 0, "it let go of controller 1 up, not stranded by the rebinding");
    a52session_bindings_reset(s);

    /* the keyboard's hold and the window's hold are separate */
    a52session_key(s, A52_KEYSYM_SPACE, 1);               /* fire from the keyboard */
    a52session_press(s, fire1, 1);                           /* and from the window */
    check(a52session_buttons_held(s, 0) == B(A52_ACT_FIRE), "fire held twice is held");
    a52session_key(s, A52_KEYSYM_SPACE, 0);
    check(a52session_buttons_held(s, 0) == B(A52_ACT_FIRE), "the key letting go leaves the window's hold");
    a52session_press(s, fire1, 0);
    check(a52session_buttons_held(s, 0) == 0, "and now nothing is");

    /* names */
    a52session_keysym_name(A52_KEYSYM_F9, name, sizeof name);
    check(!strcmp(name, "F9"), "F9 is named");
    check(!strcmp(a52_target_name(start), "START") && !strcmp(a52_target_name(up1), "Controller 1: Up"), "target names");

    a52session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
