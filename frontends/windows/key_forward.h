/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift or the layout is doing --
 * and the rule every window of the app shares for handing a key to the
 * console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "a52session.h"

static inline uint32_t a52_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned scancode = (unsigned)((lp >> 16) & 0xff);
    const int extended = (lp >> 24) & 1;
    uint32_t ks = a52session_keysym_from_win_scancode(scancode, extended);
    if (ks) return ks;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vk) {
    case VK_UP: return A52_KEYSYM_UP;
    case VK_DOWN: return A52_KEYSYM_DOWN;
    case VK_LEFT: return A52_KEYSYM_LEFT;
    case VK_RIGHT: return A52_KEYSYM_RIGHT;
    case VK_ESCAPE: return A52_KEYSYM_ESCAPE;
    case VK_RETURN: return A52_KEYSYM_RETURN;
    case VK_SPACE: return A52_KEYSYM_SPACE;
    case VK_TAB: return A52_KEYSYM_TAB;
    case VK_BACK: return A52_KEYSYM_BACKSPACE;
    case VK_SHIFT: return A52_KEYSYM_LSHIFT;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return A52_KEYSYM_F1 + (uint32_t)(vk - VK_F1);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    /* The numeric keypad keeps its own keysyms: the stick's diagonals live there. */
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return A52_KEYSYM_KP_0 + (uint32_t)(vk - VK_NUMPAD0);
    return 0;
}

/* The app's own keys, never the console's: F6 (Help), F9 (Controllers), F11
 * (Fullscreen), F12 (Debugger), Shift+F5 (Power Cycle), Ctrl+Shift+O / W / Q
 * (Open, Warp, Quit) and Ctrl+, (Preferences). Shift and Ctrl themselves
 * stay the console's: Left Shift is the second fire button. */
static inline int a52_app_key(WPARAM vk)
{
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_F6 || vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 1;
    if (vk == VK_F5 && shift) return 1;
    if (ctrl && shift && (vk == 'O' || vk == 'W' || vk == 'Q')) return 1;
    return ctrl && vk == VK_OEM_COMMA;
}

/* A key press or release for the console from any of the app's windows: 1
 * when the console took it (it is bound). Windows reports Alt, and every key
 * pressed while Alt is down, as a system key; a bound one is the console's
 * and the rest (Alt+F4, the menu mnemonics, Alt+Esc) stay Windows'. An
 * auto-repeat of a bound key is swallowed without being pressed again. The
 * session's own actions (the Power Cycle key) are the caller's. */
static inline int a52_forward_key(a52session *s, UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const uint32_t ks = a52_keysym_from_msg(vk, lp);
    if (!ks) return 0;
    if (!down)
        return a52session_key(s, ks, 0) != 0;     /* always: a key let go after a shortcut must not stick */
    if (a52_app_key(vk) || (msg == WM_SYSKEYDOWN && vk == VK_F4)) return 0;
    if (lp & (1 << 30)) return a52session_target_for_key(s, ks) >= 0;
    return a52session_key(s, ks, 1) != 0;
}
