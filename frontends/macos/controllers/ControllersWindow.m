/*
 * A52ControllersWindow -- the AppKit Controllers panel: one CX52 controller
 * per port the console has (four, or two with the 2-port BIOS) drawn side by
 * side from the session's layout (a52session_controls_layout), so every
 * frontend shows the same controller: the black wedge with START / PAUSE /
 * RESET along the top, the analog stick in its dish, the two pairs of red
 * fire buttons on the side edges, the silver "ATARI 5200" band and the
 * recessed 12-key pad with its overlay slot. A line under them says the
 * keypad and FIRE 2 are shared (atari800 scans one of each): a key held on
 * any controller lights on all of them. Then the gamepads and the controller
 * each plays, the controller the mouse drives in the picture, and Map mode.
 * The art follows ~/Workspace/fujinet-go-all/controllers-port/DESIGN-5200.md
 * (colours are the A52SESSION_*_RGB defines, read from photos).
 *
 * Gestures: press a fire button, a key or START / PAUSE / RESET and it is held
 * until it is released or dragged off; press in a stick's dish and drag to
 * steer it (analog: the deflection follows the distance from the centre; it
 * returns to the centre on release); the wheel over a stick nudges it. The
 * pointer is its own input source, so the panel lets go of everything the
 * mouse holds when it is hidden or loses key status (a52session_release_
 * pointer). What the console sees held, from any source (the host keyboard,
 * a gamepad, the mouse), is painted in orange (A52SESSION_HELD_RGB), so the
 * panel doubles as an input tester for a new gamepad.
 *
 * Keys typed with the panel key go to the console as in the main window
 * (A52ForwardKey); no view here takes the keyboard, so the panel itself is
 * the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * the number of controllers (the BIOS) and more pads than the two rows kept
 * for them change its height (the top edge stays put). Hidden at start,
 * ordered out rather than closed, so its position survives. The 30 Hz poll
 * runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (a
 * stick direction or its label, a fire button, a key) or choose nothing and
 * press a key; every control other than the pick dims to 40%. Then press a
 * key or a gamepad button; the next one is bound and whatever held it is told
 * so in the hint line. Clear unbinds the pick's key and pad button. Esc backs
 * out of the pick. F6, F9, F11, F12 and Shift+F5 are the app's and never
 * bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         11.0     /* points per layout unit */
#define MIN_WIN_W    600.0    /* the rows under the picture need this much */
#define MARGIN       16.0
#define STAGE_PAD    10.0
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W       130.0
#define POPUP_W     130.0
#define BTN_H        28.0
#define MIN_PAD_ROWS  2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS  4
#define SLOW_TICKS    8       /* the 30 Hz tick's 250 ms part */
#define PILL_H       16.0
#define MAX_PILLS   128
#define DIM_ALPHA    0.4      /* controls that are not the Map pick */
#define WHEEL_STEP   40       /* the wheel's step of the stick, of 127 */

/* ---- colours: the shared A52SESSION_*_RGB, and the stage behind them ---------- */

#define COL_STAGE_TOP   0x6B6C72
#define COL_STAGE_BOT   0x55565B
#define COL_STAGE_EDGE  0x3A3B3F
#define COL_CAPTION     0xEEEEF2
#define COL_PILL        0x1A1A1C
#define COL_CABLE       0x101012

/* ---- helpers -------------------------------------------------------------------- */

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a52session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

static uint32_t mixKey(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* Toggled-on look for a push button: accent bezel, white title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = A52AccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A linear gradient clipped to a path, from a to b. */
static void fillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, uint32_t rgbB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, 1.0,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)), topRgb, botRgb);
}

/* Lines of text centred in rect, the font shrunk (to 5 pt) until they fit. */
static void drawFittedStyled(NSString *text, NSRect rect, CGFloat size, BOOL bold, BOOL oblique, NSColor *ink)
{
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        NSMutableDictionary *dict = [NSMutableDictionary dictionaryWithDictionary:@{
            NSFontAttributeName: font,
            NSParagraphStyleAttributeName: para,
            NSForegroundColorAttributeName: ink }];
        if (oblique) dict[NSObliquenessAttributeName] = @(0.2);
        attrs = dict;
        const NSSize extent = [text sizeWithAttributes:attrs];
        lineHeight = extent.height;
        if ((extent.width <= rect.size.width && lineHeight <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    const CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight) / 2);
    [text drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
}

static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    drawFittedStyled(text, rect, size, bold, NO, ink);
}

static void fillOval(NSRect rect, NSColor *colour)
{
    [colour setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}

static void strokeLine(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:from];
    [line lineToPoint:to];
    line.lineWidth = width;
    line.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [line stroke];
}

static NSRect discRect(NSPoint ctr, CGFloat rad)
{
    return NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad);
}

/* A polygon with rounded corners (radius rad). */
static NSBezierPath *roundedPoly(const NSPoint *pts, int num, CGFloat rad)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    const NSPoint last = pts[num - 1];
    [path moveToPoint:NSMakePoint((last.x + pts[0].x) / 2, (last.y + pts[0].y) / 2)];
    for (int idx = 0; idx < num; idx++)
        [path appendBezierPathWithArcFromPoint:pts[idx] toPoint:pts[(idx + 1) % num] radius:rad];
    [path closePath];
    return path;
}

/* A label on a dark pill, filled with the accent when picked. */
static void drawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rect.size.height / 2
                                                         yRadius:rect.size.height / 2];
    [(picked ? A52AccentColor() : rgba(COL_PILL, 0.88)) setFill];
    [pill fill];
    drawFitted(text, NSInsetRect(rect, 4, 1), 10, YES, NSColor.whiteColor);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface A52CtlFlippedView : NSView
@end

@implementation A52CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class A52ControllersWindow;
@interface A52ControllersPanel : NSPanel
@property (nonatomic, weak) A52ControllersWindow *owner;
@end

@interface A52ControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} Pill;

/* The stage: every controller on a grey backdrop. Everything is drawn here and
 * hit-tested by the core (the stick's drag maths are the core's too). */
@interface A52StageView : A52CtlFlippedView
@property (nonatomic) a52session *session;
@property (nonatomic, weak) A52ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic) CGFloat winWidth;      /* the panel's content width */
@property (nonatomic, readonly) NSSize contentSize;
@property (nonatomic, readonly) int ports;   /* the controllers drawn */
- (void)configure;
/* Whether the layout is out of date: the BIOS has another number of ports. */
- (BOOL)layoutStale;
/* Reads what the console sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation A52StageView {
    a52_control _ctls[A52_CONTROLS_MAX];
    int _count;
    float _dimW, _dimH;
    int _nports;
    NSSize _size;

    unsigned _held[A52_PORTS];   /* what the console sees held, bit per a52_action */
    unsigned _keys;              /* bit per a52_key */
    int _stk[A52_PORTS][2];      /* each stick's deflection, -127..127 */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downTarget;             /* the target the pointer holds with a press, or -1 */
    BOOL _stickDrag;
    int _hover;                  /* the pressable part under the pointer (Map), or -1 */
    CGFloat _scrollAcc;
    int _wheelPort;              /* the stick the wheel is holding off centre, or -1 */
    int _wheelY;
    NSTimer *_wheelTimer;
    NSTrackingArea *_tracking;

    /* drawing state, per controller */
    NSBezierPath *_bodyPath;
    NSRect _dish[A52_PORTS];
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    _hover = -1;
    _wheelPort = -1;
    _winWidth = MIN_WIN_W;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }
- (int)ports { return _nports; }

static int activePorts(a52session *session)
{
    int nports = a52session_active_ports(session);
    if (nports < 1) nports = 1;
    if (nports > A52_PORTS) nports = A52_PORTS;
    return nports;
}

/* (Re)reads the layout for the ports the console has. */
- (void)configure
{
    _nports = activePorts(_session);
    _count = a52session_controls_layout(_nports, _ctls, A52_CONTROLS_MAX, &_dimW, &_dimH);
    if (_count < 0) _count = 0;
    _winWidth = fmax(MIN_WIN_W, ceil(_dimW * UNIT) + 2 * STAGE_PAD);
    _size = NSMakeSize(_winWidth, STAGE_PAD * 2 + ceil(_dimH * UNIT));
    _hover = -1;
    [self setNeedsDisplay:YES];
}

- (BOOL)layoutStale
{
    return activePorts(_session) != _nports;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((_winWidth - _dimW * UNIT) / 2), STAGE_PAD);
}

- (NSRect)rectOf:(const a52_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * UNIT, org.y + ctl->y * UNIT, ctl->w * UNIT, ctl->h * UNIT);
}

- (const a52_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable part under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return a52session_controls_hit(_ctls, _count, (float)((pnt.x - org.x) / UNIT), (float)((pnt.y - org.y) / UNIT));
}

/* Whether the console sees a target held (the window is a gamepad tester). */
- (BOOL)heldTarget:(int)target
{
    if (self.mapping || target < 0) return NO;
    if (target < A52_PORTS * A52_ACT_PER_PORT)
        return ((_held[target / A52_ACT_PER_PORT] >> (target % A52_ACT_PER_PORT)) & 1u) != 0;
    if (target < A52_TARGET_SYSACT(0))
        return ((_keys >> (target - A52_TARGET_KEY(0))) & 1u) != 0;
    return NO;
}

/* Whether a stick is off centre. */
- (BOOL)stickHeld:(int)port
{
    if (self.mapping || port < 0 || port >= A52_PORTS) return NO;
    return _stk[port][0] != 0 || _stk[port][1] != 0;
}

/* Whether a Map pick is one of the port's four directions. */
static BOOL isDirectionOf(int target, int port)
{
    return target >= A52_TARGET_PORT(port, A52_ACT_UP) && target <= A52_TARGET_PORT(port, A52_ACT_RIGHT);
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

- (BOOL)stickPicked:(int)port
{
    return self.mapping && self.mapTarget >= 0 && isDirectionOf(self.mapTarget, port);
}

/* Whether a part is drawn faint: in Map mode every pressable part but the pick
 * (or, while nothing is picked yet, the one under the pointer). */
- (BOOL)dimmedIdx:(int)idx
{
    if (!self.mapping) return NO;
    const a52_control *ctl = &_ctls[idx];
    switch (ctl->kind) {
    case A52_CTL_STICK:
    case A52_CTL_DISH:
        if ([self stickPicked:ctl->port]) return NO;
        break;
    case A52_CTL_FIRE:
    case A52_CTL_CONSOLE_KEY:
    case A52_CTL_KEYCAP:
        if ([self picked:ctl->target]) return NO;
        break;
    default:
        return NO;
    }
    if (self.mapTarget >= 0) return YES;
    if (_hover < 0) return YES;
    const a52_control *over = &_ctls[_hover];
    if (ctl->kind == A52_CTL_DISH) return !(over->kind == A52_CTL_STICK && over->port == ctl->port);
    return idx != _hover && !(ctl->kind != A52_CTL_STICK && over->target >= 0 && over->target == ctl->target);
}

- (void)beginDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, DIM_ALPHA);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* ---- the labels shown over the controls ---------------------------------------- */

- (int)pills:(Pill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define ADD_PILL(cx, cy, wid, tgt, prefix) do { \
        if (num < max) { \
            Pill *pill = &out[num++]; \
            pill->rect = NSMakeRect((cx) - (wid) / 2, (cy) - PILL_H / 2, (wid), PILL_H); \
            pill->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(pill->text, text.UTF8String, sizeof pill->text - 1); \
            pill->text[sizeof pill->text - 1] = '\0'; \
        } } while (0)
    const NSPoint org = [self origin];
    for (int idx = 0; idx < _count; idx++) {
        const a52_control *ctl = &_ctls[idx];
        const NSRect rect = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
        switch (ctl->kind) {
        case A52_CTL_STICK: {
            const int port = ctl->port;
            ADD_PILL(ctr.x - 2.1 * UNIT, ctr.y, 50.0, A52_TARGET_PORT(port, A52_ACT_LEFT), @"◀ ");
            ADD_PILL(ctr.x + 2.1 * UNIT, ctr.y, 50.0, A52_TARGET_PORT(port, A52_ACT_RIGHT), @"▶ ");
            ADD_PILL(ctr.x, ctr.y - 1.7 * UNIT, 50.0, A52_TARGET_PORT(port, A52_ACT_UP), @"▲ ");
            ADD_PILL(ctr.x, ctr.y + 1.7 * UNIT, 50.0, A52_TARGET_PORT(port, A52_ACT_DOWN), @"▼ ");
            break;
        }
        case A52_CTL_FIRE:
            /* one pill per pair (the left and right buttons are one wire), above the dish:
             * FIRE on the left, FIRE 2 on the right */
            if (ctl->arg == 0) {
                const BOOL fire2 = ctl->target == A52_TARGET_PORT(ctl->port, A52_ACT_FIRE2);
                const CGFloat cx = org.x + (ctl->x + 0.7f + (fire2 ? 9.1f : 2.9f)) * UNIT;
                ADD_PILL(cx, org.y + 5.4f * UNIT, 58.0, ctl->target, fire2 ? @"FIRE 2 " : @"FIRE ");
            }
            break;
        case A52_CTL_CONSOLE_KEY:
            ADD_PILL(ctr.x, NSMaxY(rect), 44.0, ctl->target, @"");
            break;
        case A52_CTL_KEYCAP:
            ADD_PILL(ctr.x, NSMaxY(rect) - 1, 30.0, ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef ADD_PILL
    return num;
}

/* The target Map would pick at a point (view points), or -1. */
- (int)mapTargetAt:(NSPoint)pnt
{
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, pills[idx].rect)) return pills[idx].target;
    const a52_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case A52_CTL_FIRE:
    case A52_CTL_CONSOLE_KEY:
    case A52_CTL_KEYCAP:
        return ctl->target;
    case A52_CTL_STICK: {
        const NSRect rect = [self rectOf:ctl];
        return a52session_stick_map_target(ctl->port, (float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                           (float)(NSWidth(rect) / 2));
    }
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    for (int port = 0; port < A52_PORTS; port++) {
        const BOOL on = !self.mapping && port < _nports;
        const unsigned held = on ? a52session_buttons_held(_session, port) : 0u;
        if (held != _held[port]) { _held[port] = held; dirty = YES; }
        int ax = 0, ay = 0;
        if (on) a52session_stick_get(_session, port, &ax, &ay);
        if (ax != _stk[port][0] || ay != _stk[port][1]) { _stk[port][0] = ax; _stk[port][1] = ay; dirty = YES; }
    }
    const unsigned keys = self.mapping ? 0u : a52session_keys_held(_session);
    if (keys != _keys) { _keys = keys; dirty = YES; }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (NSString *)legendOfTarget:(int)target
{
    if (!(self.mapping || self.showKeys) || target < 0) return nil;
    return [self.owner legendForTarget:target];
}

/* A held part: the ring and a 35% fill of the held colour. */
- (void)markHeld:(NSBezierPath *)path
{
    [rgba(A52SESSION_HELD_RGB, 0.35) setFill];
    [path fill];
    [rgb(A52SESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The picked part in Map mode: the ring. */
- (void)ringPicked:(NSBezierPath *)path
{
    [rgb(A52SESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A soft shadow on the stage under a shape. */
- (void)shadowOf:(NSBezierPath *)path
{
    NSBezierPath *shadow = [path copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:3];
    [shadow transformUsingAffineTransform:move];
    [rgba(0x000000, 0.35) setFill];
    [shadow fill];
}

/* The cable leaving the stick end. */
- (void)drawCable:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1.5, 0)
                                                         xRadius:3 yRadius:3];
    [rgb(COL_CABLE) setFill];
    [path fill];
    strokeLine(NSMakePoint(NSMinX(rect) + 3.5, NSMinY(rect) + 2), NSMakePoint(NSMinX(rect) + 3.5, NSMaxY(rect)), 1,
               rgba(A52SESSION_CASE_BEVEL_RGB, 0.8));
}

/* The black wedge: wide at the stick end, A52_BODY_BOTTOM_W at the keypad end. */
- (void)drawBody:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const CGFloat inset = (NSWidth(rect) - A52_BODY_BOTTOM_W * UNIT) / 2;
    const NSPoint pts[4] = {
        NSMakePoint(NSMinX(rect), NSMinY(rect)), NSMakePoint(NSMaxX(rect), NSMinY(rect)),
        NSMakePoint(NSMaxX(rect) - inset, NSMaxY(rect)), NSMakePoint(NSMinX(rect) + inset, NSMaxY(rect)) };
    NSBezierPath *path = roundedPoly(pts, 4, 1.2 * UNIT);
    _bodyPath = path;
    [self shadowOf:path];
    fillGradient(path, A52SESSION_CASE_BEVEL_RGB, A52SESSION_CASE_RGB);
    /* the moulded edge: a bright line inside the outline, a deep one outside */
    [rgb(A52SESSION_CASE_SHADOW_RGB) setStroke];
    path.lineWidth = 1.5;
    [path stroke];
    NSAffineTransform *shrink = [NSAffineTransform transform];
    const NSPoint mid = NSMakePoint(NSMidX(rect), NSMidY(rect));
    [shrink translateXBy:mid.x yBy:mid.y];
    [shrink scaleXBy:0.955 yBy:0.975];
    [shrink translateXBy:-mid.x yBy:-mid.y];
    NSBezierPath *inner = [path copy];
    [inner transformUsingAffineTransform:shrink];
    [rgba(A52SESSION_CASE_BEVEL_RGB, 0.9) setStroke];
    inner.lineWidth = 1;
    [inner stroke];
}

/* The brushed aluminium strip across the body, clipped to the wedge. */
- (void)drawBand:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    if (_bodyPath) {
        [NSGraphicsContext saveGraphicsState];
        [_bodyPath addClip];
    }
    NSBezierPath *strip = [NSBezierPath bezierPathWithRect:rect];
    fillGradient(strip, A52SESSION_BAND_HI_RGB, A52SESSION_BAND_LO_RGB);
    /* brush lines */
    for (CGFloat ypos = NSMinY(rect) + 2; ypos < NSMaxY(rect) - 1; ypos += 3)
        strokeLine(NSMakePoint(NSMinX(rect), ypos), NSMakePoint(NSMaxX(rect), ypos), 0.5, rgba(0xFFFFFF, 0.18));
    strokeLine(NSMakePoint(NSMinX(rect), NSMinY(rect)), NSMakePoint(NSMaxX(rect), NSMinY(rect)), 1,
               rgba(A52SESSION_CASE_SHADOW_RGB, 0.7));
    strokeLine(NSMakePoint(NSMinX(rect), NSMaxY(rect)), NSMakePoint(NSMaxX(rect), NSMaxY(rect)), 1,
               rgba(A52SESSION_CASE_SHADOW_RGB, 0.7));
    if (_bodyPath) [NSGraphicsContext restoreGraphicsState];
}

/* "ATARI 5200" on the band, the Atari fuji embossed at its left and the
 * rainbow line under the text. */
- (void)drawBandCaption:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    drawFittedStyled(nsstr(ctl->label), NSMakeRect(NSMinX(rect), NSMinY(rect) - 1, NSWidth(rect), NSHeight(rect) - 2), 11,
                     YES, NO, rgb(A52SESSION_BAND_TEXT_RGB));
    /* the rainbow: four stripes under the text */
    const uint32_t stripes[4] = { A52SESSION_RAINBOW_ORANGE_RGB, A52SESSION_RAINBOW_YELLOW_RGB,
                                  A52SESSION_RAINBOW_BLUE_RGB, A52SESSION_RAINBOW_GREEN_RGB };
    const CGFloat segW = NSWidth(rect) / 4;
    for (int idx = 0; idx < 4; idx++) {
        [rgb(stripes[idx]) setFill];
        NSRectFill(NSMakeRect(NSMinX(rect) + idx * segW, NSMaxY(rect) - 1.5, ceil(segW), 2.5));
    }
    /* the fuji: a centre bar and two outer strokes flaring at the foot */
    const NSPoint ctr = NSMakePoint(NSMinX(rect) - 1.6 * UNIT, NSMidY(rect) - 1);
    const CGFloat high = 0.55 * UNIT;
    NSColor *ink = rgba(A52SESSION_BAND_TEXT_RGB, 0.85);
    strokeLine(NSMakePoint(ctr.x, ctr.y - high), NSMakePoint(ctr.x, ctr.y + high), 2, ink);
    for (int side = -1; side <= 1; side += 2) {
        NSBezierPath *arm = [NSBezierPath bezierPath];
        [arm moveToPoint:NSMakePoint(ctr.x + side * 3, ctr.y - high)];
        [arm curveToPoint:NSMakePoint(ctr.x + side * 0.65 * UNIT, ctr.y + high)
            controlPoint1:NSMakePoint(ctr.x + side * 3, ctr.y)
            controlPoint2:NSMakePoint(ctr.x + side * 0.4 * UNIT, ctr.y + high * 0.6)];
        arm.lineWidth = 2;
        arm.lineCapStyle = NSLineCapStyleRound;
        [ink setStroke];
        [arm stroke];
    }
}

/* START / PAUSE / RESET: a black key with an italic bold legend in its colour. */
- (void)drawConsoleKey:(const a52_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const BOOL held = [self heldTarget:ctl->target];
    if (held) rect.origin.y += 1;
    uint32_t ink = A52SESSION_PAUSE_RGB;
    if (ctl->target == A52_TARGET_KEY(A52_KEY_START)) ink = A52SESSION_START_RGB;
    else if (ctl->target == A52_TARGET_KEY(A52_KEY_RESET)) ink = A52SESSION_RESET_RGB;
    NSBezierPath *bezel = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:5 yRadius:5];
    [rgb(A52SESSION_CASE_SHADOW_RGB) setFill];
    [bezel fill];
    NSBezierPath *key = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    fillGradient(key, A52SESSION_CASE_BEVEL_RGB, A52SESSION_KEY_RGB);
    NSBezierPath *face = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1, 1) xRadius:3 yRadius:3];
    fillGradient(face, mixKey(A52SESSION_KEY_RGB, 0xFFFFFF, 0.06), A52SESSION_KEY_RGB);
    drawFittedStyled(nsstr(ctl->label), NSInsetRect(rect, 2, 1), 9, YES, YES, rgb(ink));
    if (held) [self markHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:6 yRadius:6]];
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -4, -4) xRadius:7 yRadius:7]];
}

/* The stick's round dish with its chamfered rim. */
- (void)drawDish:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    if (ctl->port >= 0 && ctl->port < A52_PORTS) _dish[ctl->port] = rect;
    NSBezierPath *outer = [NSBezierPath bezierPathWithOvalInRect:rect];
    fillGradient(outer, A52SESSION_DISH_RIM_HI_RGB, A52SESSION_DISH_RIM_LO_RGB);
    NSBezierPath *inner = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 0.12 * NSWidth(rect) / 2,
                                                                             0.12 * NSHeight(rect) / 2)];
    fillGradient(inner, A52SESSION_DISH_RIM_LO_RGB, A52SESSION_CASE_SHADOW_RGB);
    [rgb(A52SESSION_CASE_SHADOW_RGB) setStroke];
    outer.lineWidth = 1;
    [outer stroke];
}

/* The stick over its dish: the ribbed rubber boot and the shaft's cap, both
 * leaning toward the deflection (the cap more). */
- (void)drawStick:(const a52_control *)ctl
{
    const int port = ctl->port;
    const NSRect dish = (port >= 0 && port < A52_PORTS) ? _dish[port] : [self rectOf:ctl];
    const CGFloat rad = NSWidth(dish) / 2;
    const NSPoint ctr = NSMakePoint(NSMidX(dish), NSMidY(dish));
    const double frx = (port >= 0 && port < A52_PORTS ? _stk[port][0] : 0) / 127.0;
    const double fry = (port >= 0 && port < A52_PORTS ? _stk[port][1] : 0) / 127.0;

    /* the boot: ribbed rings, shifted a little toward the tilt */
    const NSPoint boot = NSMakePoint(ctr.x + frx * 0.08 * rad, ctr.y + fry * 0.08 * rad);
    fillOval(discRect(boot, 0.7 * rad), rgb(A52SESSION_STICK_BOOT_RGB));
    for (int ring = 0; ring < 3; ring++) {
        NSBezierPath *rib = [NSBezierPath bezierPathWithOvalInRect:discRect(boot, (0.62 - 0.13 * ring) * rad)];
        rib.lineWidth = 1;
        [rgba(A52SESSION_CASE_BEVEL_RGB, 0.75) setStroke];
        [rib stroke];
    }
    /* the shaft's cap, up to 0.35 of the dish toward the tilt */
    const NSPoint cap = NSMakePoint(ctr.x + frx * 0.35 * rad, ctr.y + fry * 0.35 * rad);
    const CGFloat capR = 0.28 * rad;
    fillOval(discRect(NSMakePoint(cap.x + frx * 1.5, cap.y + fry * 1.5 + 1.5), capR), rgba(0x000000, 0.4));
    fillOval(discRect(cap, capR), rgb(A52SESSION_STICK_SHAFT_RGB));
    for (int rib = 0; rib < 8; rib++) {
        const double ang = rib * M_PI / 4;
        strokeLine(NSMakePoint(cap.x + 0.45 * capR * cos(ang), cap.y + 0.45 * capR * sin(ang)),
                   NSMakePoint(cap.x + 0.95 * capR * cos(ang), cap.y + 0.95 * capR * sin(ang)), 0.8,
                   rgba(A52SESSION_CASE_BEVEL_RGB, 0.9));
    }
    NSBezierPath *capRing = [NSBezierPath bezierPathWithOvalInRect:discRect(cap, capR * 0.45)];
    capRing.lineWidth = 0.8;
    [rgba(A52SESSION_CASE_BEVEL_RGB, 0.9) setStroke];
    [capRing stroke];
    NSBezierPath *capOut = [NSBezierPath bezierPathWithOvalInRect:discRect(cap, capR)];
    capOut.lineWidth = 1;
    [rgb(A52SESSION_DISH_RIM_LO_RGB) setStroke];
    [capOut stroke];
    fillOval(NSMakeRect(cap.x - 0.25 * capR, cap.y - 0.65 * capR, 0.5 * capR, 0.3 * capR), rgba(0xFFFFFF, 0.12));

    if ([self stickHeld:port])
        [self markHeld:[NSBezierPath bezierPathWithOvalInRect:discRect(cap, capR * 1.35)]];
    if ([self stickPicked:port]) {
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(dish, -3, -3)]];
        /* a mark on the rim where the picked direction is */
        const int dir = self.mapTarget % A52_ACT_PER_PORT;
        const CGFloat dx = dir == A52_ACT_LEFT ? -1 : dir == A52_ACT_RIGHT ? 1 : 0;
        const CGFloat dy = dir == A52_ACT_UP ? -1 : dir == A52_ACT_DOWN ? 1 : 0;
        fillOval(discRect(NSMakePoint(ctr.x + dx * rad, ctr.y + dy * rad), 4), rgb(A52SESSION_HELD_RGB));
    }
}

/* One of the red side buttons; both buttons of a pair light together. */
- (void)drawFire:(const a52_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const BOOL held = [self heldTarget:ctl->target];
    if (held) rect.origin.x += (ctl->arg ? 0.5 : -0.5);
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    NSBezierPath *shadow = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rect, 0, 1.5) xRadius:4 yRadius:4];
    [rgba(0x000000, 0.45) setFill];
    [shadow fill];
    fillLinear(path, NSMakePoint(NSMinX(rect), NSMinY(rect)), NSMakePoint(NSMaxX(rect), NSMaxY(rect)),
               A52SESSION_FIRE_HI_RGB, A52SESSION_FIRE_RGB);
    [rgba(0x000000, 0.45) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    fillOval(NSMakeRect(NSMinX(rect) + 2.5, NSMinY(rect) + 2, NSWidth(rect) * 0.35, 3), rgba(0xFFFFFF, 0.25));
    if (held) [self markHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:6 yRadius:6]];
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -4, -4) xRadius:7 yRadius:7]];
}

/* The recessed keypad well. */
- (void)drawWell:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    [rgb(A52SESSION_WELL_RGB) setFill];
    [path fill];
    [rgb(A52SESSION_CASE_SHADOW_RGB) setStroke];
    path.lineWidth = 2;
    [path stroke];
    strokeLine(NSMakePoint(NSMinX(rect) + 5, NSMaxY(rect) + 0.5), NSMakePoint(NSMaxX(rect) - 5, NSMaxY(rect) + 0.5), 1,
               rgba(A52SESSION_CASE_BEVEL_RGB, 0.9));
}

/* One of the pad's twelve keys, yellow legend; lit on every controller. */
- (void)drawKeycap:(const a52_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const BOOL held = [self heldTarget:ctl->target];
    if (held) rect.origin.y += 1;
    NSBezierPath *shadow = [NSBezierPath bezierPathWithRoundedRect:NSOffsetRect(rect, 0, 1.5) xRadius:4 yRadius:4];
    [rgba(0x000000, 0.5) setFill];
    [shadow fill];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    fillGradient(path, A52SESSION_CASE_BEVEL_RGB, A52SESSION_KEY_RGB);
    NSBezierPath *face = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1.5, 1.5) xRadius:3 yRadius:3];
    fillGradient(face, mixKey(A52SESSION_KEY_RGB, 0xFFFFFF, 0.07), A52SESSION_KEY_RGB);
    drawFitted(nsstr(ctl->label), NSInsetRect(rect, 2, 1), 14, YES, rgb(A52SESSION_LEGEND_RGB));
    if (held) [self markHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:6 yRadius:6]];
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -4, -4) xRadius:7 yRadius:7]];
}

- (void)drawSlot:(const a52_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    [rgb(A52SESSION_SLOT_RGB) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:NSHeight(rect) / 2 yRadius:NSHeight(rect) / 2] fill];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, _size.width, _size.height)
                                                          xRadius:10 yRadius:10];
    fillGradient(stage, COL_STAGE_TOP, COL_STAGE_BOT);
    [rgb(COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    _bodyPath = nil;
    for (int idx = 0; idx < _count; idx++) {
        const a52_control *ctl = &_ctls[idx];
        const BOOL dim = [self dimmedIdx:idx];
        [self beginDim:dim];
        switch (ctl->kind) {
        case A52_CTL_CABLE:
            [self drawCable:ctl];
            break;
        case A52_CTL_BODY:
            [self drawBody:ctl];
            break;
        case A52_CTL_BAND:
            [self drawBand:ctl];
            break;
        case A52_CTL_CAPTION:
            if (ctl->port >= 0 && ctl->label && !strcmp(ctl->label, "ATARI 5200"))
                [self drawBandCaption:ctl];
            else {
                NSRect rect = [self rectOf:ctl];
                rect = NSInsetRect(rect, 0, -3);
                drawFitted(nsstr(ctl->label), rect, 11, YES, rgb(COL_CAPTION));
            }
            break;
        case A52_CTL_CONSOLE_KEY:
            [self drawConsoleKey:ctl];
            break;
        case A52_CTL_DISH:
            [self drawDish:ctl];
            break;
        case A52_CTL_STICK:
            [self drawStick:ctl];
            break;
        case A52_CTL_FIRE:
            [self drawFire:ctl];
            break;
        case A52_CTL_WELL:
            [self drawWell:ctl];
            break;
        case A52_CTL_KEYCAP:
            [self drawKeycap:ctl];
            break;
        case A52_CTL_SLOT:
            [self drawSlot:ctl];
            break;
        case A52_CTL_NOTE: {
            NSRect rect = NSInsetRect([self rectOf:ctl], -2 * UNIT, -3);
            drawFitted(nsstr(ctl->label), rect, 10, NO, rgba(0xFFFFFF, 0.8));
            break;
        }
        default:
            break;
        }
        [self endDim:dim];
    }

    /* the Map / Show bindings labels, over everything */
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        drawPill(pills[idx].rect, nsstr(pills[idx].text), [self picked:pills[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)cancelWheel
{
    [_wheelTimer invalidate];
    _wheelTimer = nil;
    if (_wheelPort >= 0) a52session_stick_set(_session, _wheelPort, 0, 0);
    _wheelPort = -1;
    _wheelY = 0;
}

- (void)letGo
{
    [self cancelWheel];
    if (_downTarget >= 0) a52session_press(_session, _downTarget, 0);
    if (_downIdx >= 0 && _downIdx < _count && _stickDrag)
        a52session_stick_set(_session, _ctls[_downIdx].port, 0, 0);
    _downIdx = -1;
    _downTarget = -1;
    _stickDrag = NO;
    _hover = -1;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer: the core turns the offset from the dish's
 * centre into an analog deflection (full at the drag region's edge). */
- (void)dragStickAt:(NSPoint)pnt
{
    const a52_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    int xval = 0, yval = 0;
    a52session_stick_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                 (float)(NSWidth(rect) / 2), &xval, &yval);
    a52session_stick_set(_session, ctl->port, xval, yval);
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a stick direction, a fire button or a key is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    [self cancelWheel];
    const int idx = [self hit:pnt];
    const a52_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case A52_CTL_STICK:
        _downIdx = idx;
        _stickDrag = YES;
        [self dragStickAt:pnt];
        break;
    case A52_CTL_FIRE:
    case A52_CTL_CONSOLE_KEY:
    case A52_CTL_KEYCAP:
        if (ctl->target < 0) break;
        _downIdx = idx;
        _downTarget = ctl->target;
        a52session_press(_session, ctl->target, 1);
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_stickDrag) {
        [self dragStickAt:pnt];
        [self poll];
        return;
    }
    /* a button or a key: dragging off lets go */
    const a52_control *ctl = [self ctlAt:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

/* In Map mode the part under the pointer stays bright until one is picked. */
- (void)mouseMoved:(NSEvent *)evt
{
    if (!self.mapping || self.mapTarget >= 0) return;
    const int idx = [self hit:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx != _hover) {
        _hover = idx;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

- (void)wheelExpired:(NSTimer *)timer
{
    (void)timer;
    [self cancelWheel];
    [self poll];
}

/* The wheel over a stick nudges it (up pushes it up) and lets it spring back
 * to the centre a moment after the last notch. */
- (void)scrollWheel:(NSEvent *)evt
{
    if (self.mapping || _downIdx >= 0) return;
    const a52_control *ctl = [self ctlAt:[self hit:[self convertPoint:[evt locationInWindow] fromView:nil]]];
    if (!ctl || ctl->kind != A52_CTL_STICK) return;
    _scrollAcc += [evt scrollingDeltaY];
    const CGFloat per = [evt hasPreciseScrollingDeltas] ? 6.0 : 1.0;
    int steps = 0;
    while (_scrollAcc >= per) { steps++; _scrollAcc -= per; }
    while (_scrollAcc <= -per) { steps--; _scrollAcc += per; }
    if (!steps) return;
    if (_wheelPort != ctl->port) {
        [self cancelWheel];
        _wheelPort = ctl->port;
    }
    _wheelY -= steps * WHEEL_STEP;      /* up is a negative y */
    if (_wheelY > 127) _wheelY = 127;
    if (_wheelY < -127) _wheelY = -127;
    a52session_stick_set(_session, ctl->port, 0, _wheelY);
    [_wheelTimer invalidate];
    _wheelTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self selector:@selector(wheelExpired:)
                                                 userInfo:nil repeats:NO];
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const a52_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return nil;
    int target = ctl->target;
    if (ctl->kind == A52_CTL_STICK) {
        const NSRect rect = [self rectOf:ctl];
        target = a52session_stick_map_target(ctl->port, (float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                             (float)(NSWidth(rect) / 2));
        if (target < 0) return [NSString stringWithFormat:@"Controller %d stick: drag to steer, the wheel nudges", ctl->port + 1];
    }
    if (target < 0) return nil;
    const a52_binding bind = a52session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"none";
    NSString *padText = bind.button != A52_PAD_BTN_NONE ? nsstr(a52_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(a52_target_name(target)), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation A52ControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is a key of the console's (or cancels a Map pick), not "close the
 * panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation A52ControllersWindow {
    a52session *_session;
    A52AppKeyHandler _appKeys;
    A52ControllersPanel *_panel;
    A52CtlFlippedView *_content;
    A52StageView *_stage;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives controller N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_helpButton;
    NSTextField *_mouseLabel;
    NSPopUpButton *_mousePort;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _winW;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(a52session *)session appKeys:(A52AppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;
    _winW = MIN_WIN_W;

    _panel = [[A52ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, MIN_WIN_W, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    A52SetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[A52CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, MIN_WIN_W, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[A52StageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = a52session_get_int(_session, "controllers_show_bindings", 0) > 0;
    [_content addSubview:_stage];

    /* Gamepads: one row per pad, its name, the controller it drives and the one
     * it is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* Automatic (connection order), then one item per controller the console has */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _helpButton = A52HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _mouseLabel = [NSTextField labelWithString:@"Mouse in the picture drives"];
    [_content addSubview:_mouseLabel];
    _mousePort = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _mousePort.target = self;
    _mousePort.action = @selector(mousePortChanged:);
    [_mousePort setRefusesFirstResponder:YES];
    [_mousePort setToolTip:@"Dragging in the picture moves this controller's stick; the right button is its FIRE"];
    [_content addSubview:_mousePort];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [self rebuildPortMenus];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    _winW = ssize.width;
    const CGFloat inner = _winW - 2 * MARGIN;
    const CGFloat padsY = ssize.height + 10;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat mouseY = mapY + BTN_H + 8;
    const CGFloat hintY = mouseY + 28;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(_winW, total)];
    [_content setFrameSize:NSMakeSize(_winW, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, rowY + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, rowY + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, rowY - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(_winW - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_mouseLabel setFrame:NSMakeRect(MARGIN, mouseY + 3, 190, 18)];
    [_mousePort setFrame:NSMakeRect(MARGIN + 196, mouseY - 1, POPUP_W + 10, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_stage setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel (the pointer source only). */
- (void)letGo
{
    [_stage letGo];
    a52session_release_pointer(_session);
}

/* The mouse popup and the pads' popups list the controllers the console has. */
- (void)rebuildPortMenus
{
    const int nports = _stage.ports;
    [_mousePort removeAllItems];
    for (int port = 0; port < nports; port++)
        [_mousePort addItemWithTitle:[NSString stringWithFormat:@"Controller %d", port + 1]];
    [self refreshMousePort];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemWithTitle:@"Automatic"];
        for (int port = 0; port < nports; port++)
            [_padAssign[row] addItemWithTitle:[NSString stringWithFormat:@"Controller %d", port + 1]];
    }
}

/* The BIOS changed the number of controllers: the picture is read again; the
 * panel changes size only then. */
- (void)syncLayout
{
    if (![_stage layoutStale]) return;
    [self letGo];
    [_stage configure];
    [self rebuildPortMenus];
    [self refreshPads];
    [self relayout];
}

- (void)refreshMousePort
{
    int port = a52session_get_int(_session, "mouse_port", 0);
    if (port < 0 || port >= _stage.ports) port = 0;
    [_mousePort selectItemAtIndex:port];
}

- (void)mousePortChanged:(NSPopUpButton *)sender
{
    a52session_set_int(_session, "mouse_port", (int)sender.indexOfSelectedItem);
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(a52session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(int)target
{
    const a52_binding bind = a52session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"no key";
    if (bind.button != A52_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, nsstr(a52_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        a52session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             nsstr(a52_target_name(state)), [self bindingText:state]];
    } else {
        a52session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a stick direction, a fire button or a key to remap it."
            : @"Hold a button or a key, drag a stick, or use the wheel on one. Keys and gamepads work here too.";
    }
    if (state != -2) {
        [self letGo];
        a52session_release_all(_session);   /* Map's key-ups must not strand a held input */
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && target >= 0 && target < A52_TARGET_COUNT) [self setMapState:target];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(a52_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    a52session_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = A52_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !a52session_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    a52session_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(a52_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    a52session_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(a52_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a52session_bindings_reset(_session);
    for (int pad = 0, n = a52session_gamepad_count(_session); pad < n; pad++)
        a52session_gamepad_assign(_session, pad, -1);
    [self refreshPads];
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    a52session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = a52session_gamepad_effective_port(_session, row);
        NSString *text = port >= 0 ? [NSString stringWithFormat:@"drives controller %d", port + 1] : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = a52session_gamepad_count(_session);
    const int nports = _stage.ports;
    _padGeneration = a52session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a52session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        /* the pad's own port, or Automatic when it has none (or one the console lacks) */
        const int assign = a52session_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= nports ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    a52session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment, and the BIOS's ports. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        [self syncLayout];
        if (a52session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
    }
    [_stage poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak A52ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)e
{
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    if (A52IsAppKey(e)) {
        if (![e isARepeat] && _appKeys) _appKeys(e);
        return;
    }
    const uint32_t ks = A52KeysymFromEvent(e);
    if (!ks) return;
    if (_swallowKey && ks == _swallowKey) return;
    if ([e isARepeat]) return;
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick; a Ctrl chord
         * and the app's keys are not bindings */
        if ([e modifierFlags] & NSEventModifierFlagControl) return;
        if (a52session_key_reserved(ks)) return;
        if (ks == A52_KEYSYM_ESCAPE) {
            [self setMapState:-1];
            _swallowKey = ks;
        } else {
            [self bindKey:ks swallow:YES];
        }
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sa = a52session_key_sysaction(_session, ks);
    if (sa >= 0) {
        a52session_sysaction_post(_session, sa);
        return;
    }
    A52ForwardKey(_session, e, YES);
}

- (void)panelKeyUp:(NSEvent *)e
{
    const uint32_t ks = A52KeysymFromEvent(e);
    if (_swallowKey && ks == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (_mapState != -2) return;
    A52ForwardKey(_session, e, NO);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)e
{
    if (_mapState >= 0) {
        /* a modifier pressed while a pick waits is the binding */
        const NSUInteger f = [e modifierFlags];
        BOOL down = NO;
        uint32_t ks = 0;
        switch ([e keyCode]) {
        case 56: ks = A52_KEYSYM_LSHIFT; down = (f & NSEventModifierFlagShift) != 0; break;
        case 60: ks = A52_KEYSYM_RSHIFT; down = (f & NSEventModifierFlagShift) != 0; break;
        case 59: ks = A52_KEYSYM_LCTRL; down = (f & NSEventModifierFlagControl) != 0; break;
        case 62: ks = A52_KEYSYM_RCTRL; down = (f & NSEventModifierFlagControl) != 0; break;
        case 58: ks = A52_KEYSYM_LALT; down = (f & NSEventModifierFlagOption) != 0; break;
        case 61: ks = A52_KEYSYM_RALT; down = (f & NSEventModifierFlagOption) != 0; break;
        default: break;
        }
        if (ks && down) [self bindKey:ks swallow:NO];
        return;
    }
    if (_mapState == -1) return;
    A52ForwardFlags(_session, e);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    A52ForwardReleaseAll(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self syncLayout];
    [self rebuildPortMenus];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (BOOL)isActive
{
    return [_panel isKeyWindow];
}

/* The BIOS, the bindings or the gamepads changed elsewhere (Settings, a
 * hot-plug): read again. */
- (void)refresh
{
    [self syncLayout];
    [self refreshPads];
    [self refreshMousePort];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    A52ForwardReleaseAll(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
