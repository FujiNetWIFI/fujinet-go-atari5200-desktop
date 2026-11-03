/*
 * bindings -- the remappable key/gamepad table and the names the bindings
 * editor needs.
 *
 * One flat target table (A52_TARGET_*): per port the stick's four
 * directions, fire and fire 2; the console keys (the 12-key pad, START,
 * PAUSE, RESET); the session's own actions. Each target holds at most one
 * keysym and one gamepad button; rebinding steals. Persisted as one
 * "bindings" key holding only what differs from the defaults.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"
#include "bindings.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[A52_TARGET_COUNT];
static slot s_defaults[A52_TARGET_COUNT];
static int s_seeded;
static struct a52session *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* Controller 1's stick on the arrow keys, fire on Space (the side
 * buttons) and fire 2 on Left Shift (the top buttons); controller 2 on WASD
 * with Left Ctrl / Left Alt for its fires would clash with typing, so ports
 * 2-4 are gamepad (or mouse-drag) only. The 12-key pad on the digit row and
 * the numeric keypad (0-9 as digits; "*" and "#" on the keypad's * and /, and
 * on "-" and "="), START / PAUSE / RESET on F4 / F3 / F5 (F1 is Help, F9 the
 * Controllers window and F12 the debugger: the frontends'), Power cycle on
 * Escape's neighbour F10? no: Escape is not bound, F5 reboots to CONFIG.
 * Gamepads: South fires, East fires 2, Start / Back are START / PAUSE and
 * North is RESET. */
static void compute_defaults_locked(void)
{
    int t, port, k;
    for (t = 0; t < A52_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = A52_PAD_BTN_NONE;
    }
    seed_key(A52_TARGET_PORT(0, A52_ACT_UP), A52_KEYSYM_UP);
    seed_key(A52_TARGET_PORT(0, A52_ACT_DOWN), A52_KEYSYM_DOWN);
    seed_key(A52_TARGET_PORT(0, A52_ACT_LEFT), A52_KEYSYM_LEFT);
    seed_key(A52_TARGET_PORT(0, A52_ACT_RIGHT), A52_KEYSYM_RIGHT);
    seed_key(A52_TARGET_PORT(0, A52_ACT_FIRE), A52_KEYSYM_SPACE);
    seed_key(A52_TARGET_PORT(0, A52_ACT_FIRE2), A52_KEYSYM_LSHIFT);

    for (k = 0; k <= 9; k++)
        seed_key(A52_TARGET_KEY(A52_KEY_0 + k), '0' + k);
    seed_key(A52_TARGET_KEY(A52_KEY_STAR), '-');
    seed_key(A52_TARGET_KEY(A52_KEY_HASH), '=');
    seed_key(A52_TARGET_KEY(A52_KEY_START), A52_KEYSYM_F4);
    seed_key(A52_TARGET_KEY(A52_KEY_PAUSE), A52_KEYSYM_F3);
    seed_key(A52_TARGET_KEY(A52_KEY_RESET), A52_KEYSYM_F5);
    /* Power Cycle is Shift+F5, handled by every frontend (not bindable). */

    for (port = 0; port < A52_PORTS; port++) {
        seed_button(A52_TARGET_PORT(port, A52_ACT_FIRE), A52_PAD_BTN_SOUTH);
        seed_button(A52_TARGET_PORT(port, A52_ACT_FIRE2), A52_PAD_BTN_EAST);
    }
    seed_button(A52_TARGET_KEY(A52_KEY_START), A52_PAD_BTN_START);
    seed_button(A52_TARGET_KEY(A52_KEY_PAUSE), A52_PAD_BTN_BACK);
    seed_button(A52_TARGET_KEY(A52_KEY_RESET), A52_PAD_BTN_NORTH);
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < A52_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < A52_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    a52session_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct a52session *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(a52session_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < A52_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int a52session_target_for_key(a52session *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int a52session_target_for_button(a52session *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == A52_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < A52_ACT_PER_PORT && port >= 0 && port < A52_PORTS; t++)
        if (s_table[A52_TARGET_PORT(port, t)].button == button) {
            found = A52_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = A52_PORTS * A52_ACT_PER_PORT; t < A52_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

a52_binding a52session_binding_get(a52session *s, int target)
{
    a52_binding b = { 0, A52_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= A52_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", a52_target_name(victims[i]));
}

void a52session_binding_set_key(a52session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[A52_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= A52_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < A52_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a52session_binding_set_button(a52session *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[A52_TARGET_COUNT], nvictims = 0, t;
    int port = target / A52_ACT_PER_PORT;   /* >= A52_PORTS: console, sysactions */
    (void)s;
    if (target < 0 || target >= A52_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != A52_PAD_BTN_NONE) {
        for (t = 0; t < A52_TARGET_COUNT; t++) {
            int tport = t / A52_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= A52_PORTS || port >= A52_PORTS)) {
                s_table[t].button = A52_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a52session_bindings_reset(a52session *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) a52session_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[A52_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "Fire", "Fire 2",
};
static const char *const key_labels[A52_KEY_COUNT] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#", "START", "PAUSE", "RESET",
};
static const char *const sysact_names[A52_SYSACT_COUNT] = {
    "Power cycle (reboot to CONFIG)", "Pause (debugger)", "Warp speed",
};

const char *a52_key_label(int key)
{
    return key >= 0 && key < A52_KEY_COUNT ? key_labels[key] : "";
}

const char *a52_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= A52_TARGET_COUNT) return "";
    if (target < A52_TARGET_KEY(0)) {
        snprintf(buf, sizeof buf, "Controller %d: %s", target / A52_ACT_PER_PORT + 1,
                 act_names[target % A52_ACT_PER_PORT]);
        return buf;
    }
    if (target < A52_TARGET_SYSACT(0)) {
        int k = target - A52_TARGET_KEY(0);
        if (k <= A52_KEY_HASH)
            snprintf(buf, sizeof buf, "Key %s", key_labels[k]);
        else
            snprintf(buf, sizeof buf, "%s", key_labels[k]);
        return buf;
    }
    return sysact_names[target - A52_TARGET_SYSACT(0)];
}

const char *a52_target_short_name(int target)
{
    if (target < 0 || target >= A52_TARGET_COUNT) return "";
    if (target < A52_TARGET_KEY(0)) return act_names[target % A52_ACT_PER_PORT];
    if (target < A52_TARGET_SYSACT(0)) return key_labels[target - A52_TARGET_KEY(0)];
    return sysact_names[target - A52_TARGET_SYSACT(0)];
}

static const char *const pad_button_names[A52_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *a52_pad_button_name(int button)
{
    static char buf[32];
    if (A52_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (A52_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - A52_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (A52_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[A52_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - A52_PAD_HAT_BASE) / A52_PAD_HAT_DIRS;
        int d = (button - A52_PAD_HAT_BASE) % A52_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int a52session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case A52_KEYSYM_UP: name = "Up"; break;
    case A52_KEYSYM_DOWN: name = "Down"; break;
    case A52_KEYSYM_LEFT: name = "Left"; break;
    case A52_KEYSYM_RIGHT: name = "Right"; break;
    case A52_KEYSYM_ESCAPE: name = "Escape"; break;
    case A52_KEYSYM_RETURN: name = "Return"; break;
    case A52_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case A52_KEYSYM_TAB: name = "Tab"; break;
    case A52_KEYSYM_SPACE: name = "Space"; break;
    case A52_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case A52_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case A52_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case A52_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case A52_KEYSYM_LALT: name = "Left Alt"; break;
    case A52_KEYSYM_RALT: name = "Right Alt"; break;
    case A52_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case A52_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case A52_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case A52_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= A52_KEYSYM_F1 && keysym <= A52_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - A52_KEYSYM_F1 + 1);
    if (keysym >= A52_KEYSYM_KP_0 && keysym <= A52_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - A52_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= A52SESSION_KEYSYM_HID_BASE
        && keysym <= A52SESSION_KEYSYM_HID_BASE + A52SESSION_HID_USAGE_MAX) {
        const char *hid = a52_hid_usage_name(keysym - A52SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - A52SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- for a52session_key ------------------------------------------------ */

int a52session_key_sysaction(a52session *s, uint32_t keysym)
{
    int t = a52session_target_for_key(s, keysym);
    if (t < A52_TARGET_SYSACT(0)) return -1;
    return t - A52_TARGET_SYSACT(0);
}

/* The joystick or console target a key drives, or -1 (system actions are
 * the frontends' to fire). */
int session_binding_target_for_key(struct a52session *s, uint32_t keysym)
{
    int t = a52session_target_for_key(s, keysym);
    return t >= 0 && t < A52_TARGET_SYSACT(0) ? t : -1;
}
