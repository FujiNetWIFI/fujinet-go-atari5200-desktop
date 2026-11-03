/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

/* key code -> the keysym its press sent, until its release */
static NSMutableDictionary<NSNumber *, NSNumber *> *s_down;

static NSMutableDictionary<NSNumber *, NSNumber *> *downKeys(void)
{
    if (!s_down) s_down = [NSMutableDictionary dictionary];
    return s_down;
}

uint32_t A52KeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = a52session_keysym_from_macos_keycode([event keyCode]);
    if (ks) return ks;

    switch ([event keyCode]) {
    case kVK_UpArrow:    return A52_KEYSYM_UP;
    case kVK_DownArrow:  return A52_KEYSYM_DOWN;
    case kVK_LeftArrow:  return A52_KEYSYM_LEFT;
    case kVK_RightArrow: return A52_KEYSYM_RIGHT;
    case kVK_Tab:        return A52_KEYSYM_TAB;
    case kVK_Escape:     return A52_KEYSYM_ESCAPE;
    case kVK_Return:     return A52_KEYSYM_RETURN;
    case kVK_Space:      return A52_KEYSYM_SPACE;
    case kVK_Delete:     return A52_KEYSYM_BACKSPACE;
    default: break;
    }

    /* Anything the table does not know: the character typed. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c <= 0x7e) {
            if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A' + 'a');
            return (uint32_t)c;
        }
        if (c >= NSF1FunctionKey && c <= NSF12FunctionKey)
            return A52_KEYSYM_F1 + (uint32_t)(c - NSF1FunctionKey);
    }
    return 0;
}

BOOL A52IsAppKey(NSEvent *event)
{
    const unsigned short k = [event keyCode];
    if (k == kVK_F6 || k == kVK_F9 || k == kVK_F11 || k == kVK_F12) return YES;
    return k == kVK_F5 && ([event modifierFlags] & NSEventModifierFlagShift);
}

/* The keysym a press sends: the key's own, except that Shift on a main-row
 * key makes the character it types ('*' on 8, '#' on 3) -- the 5200's keypad
 * has both. */
static uint32_t pressKeysym(NSEvent *event)
{
    uint32_t ks = A52KeysymFromEvent(event);
    if (ks >= 0x20 && ks <= 0x7e && ([event modifierFlags] & NSEventModifierFlagShift) &&
        !(ks >= 'a' && ks <= 'z')) {
        NSString *typed = [event characters];
        if ([typed length] == 1) {
            const unichar c = [typed characterAtIndex:0];
            if (c >= 0x20 && c <= 0x7e) ks = (uint32_t)c;
        }
    }
    return ks;
}

BOOL A52ForwardKey(a52session *session, NSEvent *event, BOOL down)
{
    NSNumber *code = @([event keyCode]);
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        NSNumber *sent = downKeys()[code];
        if (sent) {
            a52session_key(session, [sent unsignedIntValue], 0);
            [downKeys() removeObjectForKey:code];
            return YES;
        }
        return NO;
    }
    /* Command shortcuts are the menus'; the app's keys are the caller's */
    if (([event modifierFlags] & NSEventModifierFlagCommand) || A52IsAppKey(event)) return NO;
    if ([event isARepeat]) return YES;
    const uint32_t ks = pressKeysym(event);
    if (!ks) return NO;
    a52session_key(session, ks, 1);
    downKeys()[code] = @(ks);
    return YES;
}

/* The device-dependent bits of an event's modifier flags, one per side (IOKit's
 * NX_DEVICE*KEYMASK): the pair's own flag stays set while either side is held,
 * so a release of one with the other down would read as a press. */
#define A52_DEV_LCTL   0x00000001UL
#define A52_DEV_LSHIFT 0x00000002UL
#define A52_DEV_RSHIFT 0x00000004UL
#define A52_DEV_LALT   0x00000020UL
#define A52_DEV_RALT   0x00000040UL
#define A52_DEV_RCTL   0x00002000UL
#define A52_DEV_ANY    (A52_DEV_LCTL | A52_DEV_LSHIFT | A52_DEV_RSHIFT | A52_DEV_LALT | A52_DEV_RALT | A52_DEV_RCTL)

/* Whether a modifier key is down: its group flag is set and, when the event
 * carries the per-side bits (a synthetic one may not), its own bit is. */
static int modifierDown(NSUInteger flags, NSUInteger group, NSUInteger side)
{
    if (!(flags & group)) return 0;
    if (!(flags & A52_DEV_ANY)) return 1;
    return (flags & side) != 0;
}

void A52ForwardFlags(a52session *session, NSEvent *event)
{
    const NSUInteger f = (NSUInteger)[event modifierFlags];
    uint32_t ks = 0;
    int down = 0;
    switch ([event keyCode]) {
    case kVK_Shift:        ks = A52_KEYSYM_LSHIFT; down = modifierDown(f, NSEventModifierFlagShift, A52_DEV_LSHIFT); break;
    case kVK_RightShift:   ks = A52_KEYSYM_RSHIFT; down = modifierDown(f, NSEventModifierFlagShift, A52_DEV_RSHIFT); break;
    case kVK_Control:      ks = A52_KEYSYM_LCTRL;  down = modifierDown(f, NSEventModifierFlagControl, A52_DEV_LCTL); break;
    case kVK_RightControl: ks = A52_KEYSYM_RCTRL;  down = modifierDown(f, NSEventModifierFlagControl, A52_DEV_RCTL); break;
    case kVK_Option:       ks = A52_KEYSYM_LALT;   down = modifierDown(f, NSEventModifierFlagOption, A52_DEV_LALT); break;
    case kVK_RightOption:  ks = A52_KEYSYM_RALT;   down = modifierDown(f, NSEventModifierFlagOption, A52_DEV_RALT); break;
    default: return;
    }
    a52session_key(session, ks, down);
}

void A52ForwardReleaseAll(a52session *session)
{
    for (NSNumber *sent in [downKeys() allValues])
        a52session_key(session, [sent unsignedIntValue], 0);
    [downKeys() removeAllObjects];
    a52session_release_all(session);
}

static NSColor *colorFromRgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

NSColor *A52AccentColor(void)
{
    return colorFromRgb(A52SESSION_ACCENT_RGB);
}

NSColor *A52AccentTextColor(NSAppearance *appearance)
{
    NSAppearanceName best = [(appearance ? appearance : [NSApp effectiveAppearance])
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return colorFromRgb([best isEqualToString:NSAppearanceNameDarkAqua]
                            ? A52SESSION_ACCENT_RGB : A52SESSION_ACCENT_DARK_RGB);
}
