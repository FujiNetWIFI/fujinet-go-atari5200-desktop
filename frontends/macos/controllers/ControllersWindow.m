/*
 * A52ControllersWindow -- the AppKit Controllers panel: the Atari 5200 drawn
 * like the console (black case, silver band, orange POWER legend, yellow
 * keypad digits): the 12-key pad with START / PAUSE / RESET from the
 * session's layout table (so every frontend shows the same caps), and four
 * controllers, each an analog stick (drag it; the knob shows where it is,
 * analog or digital) with its FIRE and FIRE 2 buttons; the gamepads and the
 * controller each drives, the controller the mouse drives in the picture,
 * and Map mode.
 *
 * One custom-drawn view (A52ConsoleView) paints the console and takes the
 * mouse. Every key cap, stick and fire button is an A52Ctl with a rectangle
 * and a binding TARGET (a52session.h): pressing it is a52session_press
 * (target), Map rebinds that target. A control acts on mouseDown AND
 * mouseUp, not on a click: the console samples its inputs once a frame, so a
 * press that lasted only the instant of a click would fall between frames. A
 * cap is HELD while the mouse is down on it, like the real key; dragging off
 * lets it go. A stick follows the mouse from its centre (a52session_stick_
 * set, analog). Hiding, losing the key window and rebuilding let go of what
 * the mouse holds here (only that: the main window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this panel -- is
 * filled with the accent (dark text), so the panel doubles as an input
 * tester for a new gamepad. Keys typed with the panel key go to the console
 * as in the main window; no view here takes the keyboard, so the panel
 * itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * a rebuild resizes it; the top edge stays put. Hidden at start, ordered out
 * rather than closed, so its position survives. The 30 Hz poll runs only
 * while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a key cap, a stick direction
 * or a fire button, then press a key or a gamepad button; the next one is
 * bound and whatever held it is told so in the hint line. Esc cancels the
 * pick; typed keys are not typed while Map is armed. Rebinding steals the key
 * from its old target.
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

#define KBD_UNIT_MAX 40.0   /* points per key unit */
#define KBD_WIDTH_MAX 300.0 /* the keypad is squeezed to this */
#define PLATE_PAD    10.0   /* the black keyboard plate around the caps */
#define MARGIN       14.0
#define BAND_H       12.0   /* the silver band along the top of the case */
#define HEADER_H     38.0   /* the band and the nameplate above the plate */
#define STICK_W     214.0
#define STICK_H     172.0
#define STICK_GAP    16.0
#define SECTION_GAP  14.0
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W       130.0
#define POPUP_W     130.0
#define BTN_H        28.0
#define MAX_PAD_ROWS  4
#define SLOW_TICKS    8     /* the 30 Hz tick's 250 ms part */

/* The kinds of control beyond the layout's a52_keycap_kind (0, 1). */
#define A52CTL_STICK 10     /* a controller's analog stick: its target is the port's UP */
#define A52CTL_FIRE  11     /* the side fire buttons */
#define A52CTL_FIRE2 12     /* the top fire buttons */
#define STICK_R      58.0   /* the stick dish's radius */

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a52session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t v)
{
    return [NSColor colorWithSRGBRed:((v >> 16) & 0xff) / 255.0
                               green:((v >> 8) & 0xff) / 255.0
                                blue:(v & 0xff) / 255.0
                               alpha:1.0];
}

/* Lines of text centred in r, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect r, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat w = 0;
        lineH = 0;
        for (NSString *l in lines) {
            const NSSize s = [l sizeWithAttributes:attrs];
            if (s.width > w) w = s.width;
            if (s.height > lineH) lineH = s.height;
        }
        if ((w <= r.size.width && lineH * nlines <= r.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat y = r.origin.y + floor((r.size.height - lineH * nlines) / 2);
    for (NSString *l in lines) {
        [l drawInRect:NSMakeRect(r.origin.x, y, r.size.width, lineH) withAttributes:attrs];
        y += lineH;
    }
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' tables. */
@interface A52CtlFlippedView : NSView
@end

@implementation A52CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* One key cap, stick or fire button. */
@interface A52Ctl : NSObject
@property (nonatomic) NSRect rect;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) int target;     /* the binding target: what a press applies and Map rebinds */
@property (nonatomic) int kind;       /* a52_keycap_kind, or A52CTL_* */
@property (nonatomic) BOOL lit;       /* held by any source */
@property (nonatomic) int port;       /* a stick's controller */
@property (nonatomic) unsigned mask;  /* a stick: directions held (bit = a52_action) */
@property (nonatomic) int ax, ay;     /* a stick: the analog deflection, -128..127 */
@end

@implementation A52Ctl
@end

/* Keys typed with the panel key go to its owner. */
@class A52ControllersWindow;
@interface A52ControllersPanel : NSPanel
@property (nonatomic, weak) A52ControllersWindow *owner;
@end

@interface A52ControllersWindow () <NSWindowDelegate>
- (void)ctlPressed:(A52Ctl *)c down:(BOOL)down;
- (void)stickDeflect:(int)x y:(int)y port:(int)port;
- (void)stickPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* The console: the black case with its silver band, the plate with the
 * keypad, and the four controllers. Everything on it is drawn here and
 * hit-tested here. */
@interface A52ConsoleView : A52CtlFlippedView
@property (nonatomic, weak) A52ControllersWindow *owner;
@property (nonatomic, copy) NSArray<A52Ctl *> *controls;
@property (nonatomic, copy) NSArray<NSValue *> *stickBoxes;   /* four rects: the controllers */
@property (nonatomic) NSRect plate;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
- (void)forgetMouse;
@end

@implementation A52ConsoleView {
    A52Ctl *_down;                            /* the control the mouse holds */
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)drawControl:(A52Ctl *)c
{
    if (c.kind == A52CTL_STICK) return;     /* drawn with its controller */
    const NSRect r = NSInsetRect(c.rect, 0.5, 0.5);
    const BOOL sel = self.mapping && c.target == self.mapTarget;
    const BOOL on = c.lit || sel;
    NSColor *accent = rgb(A52SESSION_ACCENT_RGB);
    NSColor *fill, *ink, *edge;
    CGFloat size = 15;

    if (c.kind == A52CTL_FIRE || c.kind == A52CTL_FIRE2) {
        fill = on ? accent : (c.kind == A52CTL_FIRE ? rgb(0x8a2a22) : rgb(0x6c3a1c));
        ink = on ? rgb(0x1a1408) : rgb(0xf4dccc);
        edge = on ? [NSColor whiteColor] : rgb(0x3a1410);
        size = 11;
    } else if (c.target == A52_TARGET_KEY(A52_KEY_RESET)) {
        fill = on ? accent : rgb(0xb6322a);
        ink = on ? rgb(0x1a1408) : [NSColor whiteColor];
        edge = on ? [NSColor whiteColor] : rgb(0x5a1a16);
        size = 11;
    } else if (c.kind == A52_KEYCAP_FUNCTION) {
        fill = on ? accent : rgb(0x4a4a50);
        ink = on ? rgb(0x1a1408) : rgb(0xe8e8ec);
        edge = on ? [NSColor whiteColor] : rgb(0x28282c);
        size = 11;
    } else {       /* a keypad key: dark, with the yellow digit */
        fill = on ? accent : rgb(0x38383c);
        ink = on ? rgb(0x1a1408) : rgb(0xe8c547);
        edge = on ? [NSColor whiteColor] : rgb(0x1c1c1f);
        size = 16;
    }

    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5];
    [fill setFill];
    [path fill];
    [edge setStroke];
    path.lineWidth = on ? 2 : 1;
    [path stroke];

    /* the bound host key as a small legend under the face, in Map mode (what
     * is bound to what) and when asked for */
    NSString *legend = nil;
    if (self.mapping || self.showKeys) legend = [self.owner legendForTarget:c.target];
    const NSRect inner = NSInsetRect(c.rect, 3, 2);
    if (legend.length) {
        const CGFloat top = floor(inner.size.height * 0.58);
        drawFitted(c.face, NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, top),
                   size, YES, ink);
        drawFitted(legend, NSMakeRect(inner.origin.x, inner.origin.y + top, inner.size.width,
                                      inner.size.height - top),
                   8, NO, on ? rgb(0x1a1408) : rgb(0xc8c8cc));
    } else {
        drawFitted(c.face, inner, size, YES, ink);
    }
}

/* One controller's stick: a dish with its four directions marked, a knob
 * where the stick is (the analog deflection when there is one, else the held
 * directions), the held directions lit, and in Map mode the direction
 * waiting for a key. */
- (void)drawStick:(A52Ctl *)c
{
    NSColor *accent = rgb(A52SESSION_ACCENT_RGB);
    const NSRect r = c.rect;
    const NSPoint mid = NSMakePoint(NSMidX(r), NSMidY(r));
    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(r, 1, 1)];
    [rgb(0x1c1c1f) setFill];
    [dish fill];
    [rgb(0x6a6a70) setStroke];
    dish.lineWidth = 2;
    [dish stroke];
    NSBezierPath *inner = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(r, 9, 9)];
    [rgb(0x2c2c30) setFill];
    [inner fill];

    static const struct { const char *glyph; int dx, dy; } dirs[A52_ACT_FIRE] = {
        { "▲", 0, -1 }, { "▼", 0, 1 }, { "◀", -1, 0 }, { "▶", 1, 0 },
    };
    for (int a = 0; a < A52_ACT_FIRE; a++) {
        const int target = A52_TARGET_PORT(c.port, a);
        const BOOL held = (c.mask >> a) & 1u;
        const BOOL sel = self.mapping && target == self.mapTarget;
        const NSRect cell = NSMakeRect(mid.x + dirs[a].dx * 40 - 15, mid.y + dirs[a].dy * 40 - 11, 30, 22);
        if (held || sel) {
            NSBezierPath *hot = [NSBezierPath bezierPathWithRoundedRect:cell xRadius:6 yRadius:6];
            [accent setFill];
            [hot fill];
            [[NSColor whiteColor] setStroke];
            hot.lineWidth = 2;
            [hot stroke];
        }
        NSString *legend = (self.mapping || self.showKeys) ? [self.owner legendForTarget:target] : nil;
        NSColor *ink = (held || sel) ? rgb(0x1a1408) : rgb(0xb4b4b8);
        if (legend.length) {
            drawFitted(nsstr(dirs[a].glyph), NSMakeRect(cell.origin.x, cell.origin.y - 4, cell.size.width, 14), 11, YES, ink);
            drawFitted(legend, NSMakeRect(cell.origin.x - 4, cell.origin.y + 10, cell.size.width + 8, 12), 8, NO, ink);
        } else {
            drawFitted(nsstr(dirs[a].glyph), cell, 14, YES, ink);
        }
    }

    /* the knob: the analog position when the pad deflects it, else the held
     * directions at the dish's ends (diagonals on the circle) */
    double kx = c.ax / 127.0, ky = c.ay / 127.0;
    if (c.ax == 0 && c.ay == 0) {
        kx = ((c.mask >> A52_ACT_RIGHT) & 1u) - ((c.mask >> A52_ACT_LEFT) & 1u);
        ky = ((c.mask >> A52_ACT_DOWN) & 1u) - ((c.mask >> A52_ACT_UP) & 1u);
        if (kx != 0 && ky != 0) { kx *= 0.7071; ky *= 0.7071; }
    }
    const double len = sqrt(kx * kx + ky * ky);
    if (len > 1.0) { kx /= len; ky /= len; }
    const double travel = STICK_R - 22;
    const NSRect knob = NSMakeRect(mid.x + kx * travel - 9, mid.y + ky * travel - 9, 18, 18);
    NSBezierPath *k = [NSBezierPath bezierPathWithOvalInRect:knob];
    [(len > 0.01 ? accent : rgb(0x0e0e10)) setFill];
    [k fill];
    [rgb(0xe8e8ec) setStroke];
    k.lineWidth = 1.5;
    [k stroke];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];

    /* the black case with its silver band */
    [rgb(0x2a2a2a) setFill];
    NSRectFill(b);
    [rgb(0x1a1a1a) setFill];
    NSRectFill(NSMakeRect(0, b.size.height - 1, b.size.width, 1));
    [rgb(0xb8b8bc) setFill];
    NSRectFill(NSMakeRect(0, 0, b.size.width, BAND_H));
    [rgb(0x8e8e93) setFill];
    for (CGFloat y = 3; y < BAND_H; y += 4) NSRectFill(NSMakeRect(0, y, b.size.width, 1));
    [rgb(0x1a1a1a) setFill];
    NSRectFill(NSMakeRect(0, BAND_H, b.size.width, 1));

    NSDictionary *nameplate = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                                 NSForegroundColorAttributeName: rgb(A52SESSION_ACCENT_RGB) };
    [self.title drawAtPoint:NSMakePoint(MARGIN, BAND_H + 8) withAttributes:nameplate];

    /* the plate under the keypad */
    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:self.plate xRadius:8 yRadius:8];
    [rgb(0x141416) setFill];
    [plate fill];

    /* the controllers' bodies and their names */
    NSDictionary *name = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                            NSForegroundColorAttributeName: rgb(0xa8a8ad) };
    for (NSUInteger i = 0; i < [self.stickBoxes count]; i++) {
        const NSRect sb = [self.stickBoxes[i] rectValue];
        NSBezierPath *base = [NSBezierPath bezierPathWithRoundedRect:sb xRadius:10 yRadius:10];
        [rgb(0x222224) setFill];
        [base fill];
        [rgb(0x3e3e44) setStroke];
        base.lineWidth = 1;
        [base stroke];
        [[NSString stringWithFormat:@"CONTROLLER %lu", (unsigned long)(i + 1)]
            drawAtPoint:NSMakePoint(sb.origin.x + 10, sb.origin.y + 8) withAttributes:name];
    }

    for (A52Ctl *c in self.controls) {
        if (c.kind == A52CTL_STICK) [self drawStick:c];
        else [self drawControl:c];
    }
}

- (A52Ctl *)controlAt:(NSPoint)p
{
    for (A52Ctl *c in self.controls) {
        if (c.kind == A52CTL_STICK) {     /* round */
            const double dx = p.x - NSMidX(c.rect), dy = p.y - NSMidY(c.rect);
            if (dx * dx + dy * dy <= STICK_R * STICK_R) return c;
        } else if (NSPointInRect(p, c.rect)) {
            return c;
        }
    }
    return nil;
}

/* The analog deflection of a point on a stick: the offset from the centre
 * over the radius, clamped to the rim, with a small dead zone (the view is
 * flipped, so +y is down, as a52session_stick_set wants). */
static void stickDeflection(A52Ctl *c, NSPoint p, int *x, int *y)
{
    double dx = (p.x - NSMidX(c.rect)) / STICK_R, dy = (p.y - NSMidY(c.rect)) / STICK_R;
    double len = sqrt(dx * dx + dy * dy);
    *x = *y = 0;
    if (len > 1.0) { dx /= len; dy /= len; len = 1.0; }
    if (len > 0.08) {
        *x = (int)lround(dx * 127.0);
        *y = (int)lround(dy * 127.0);
    }
}

- (void)mouseDown:(NSEvent *)e
{
    if (_down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    A52Ctl *c = [self controlAt:p];
    if (!c) return;
    if (c.kind == A52CTL_STICK) {
        if (self.mapping) {
            /* Map: the quadrant clicked is the direction to rebind */
            const double dx = p.x - NSMidX(c.rect), dy = p.y - NSMidY(c.rect);
            const int act = fabs(dx) > fabs(dy) ? (dx > 0 ? A52_ACT_RIGHT : A52_ACT_LEFT)
                                                : (dy > 0 ? A52_ACT_DOWN : A52_ACT_UP);
            [self.owner stickPick:A52_TARGET_PORT(c.port, act)];
            return;
        }
        _down = c;
        int ax, ay;
        stickDeflection(c, p, &ax, &ay);
        [self.owner stickDeflect:ax y:ay port:c.port];
        return;
    }
    _down = c;
    [self.owner ctlPressed:c down:YES];
}

/* Dragging off a control must let it go; a stick follows the mouse. */
- (void)mouseDragged:(NSEvent *)e
{
    if (!_down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (_down.kind == A52CTL_STICK) {
        int ax, ay;
        stickDeflection(_down, p, &ax, &ay);
        [self.owner stickDeflect:ax y:ay port:_down.port];
        return;
    }
    if (NSPointInRect(p, _down.rect)) return;
    A52Ctl *c = _down;
    _down = nil;
    [self.owner ctlPressed:c down:NO];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!_down) return;
    A52Ctl *c = _down;
    _down = nil;
    if (c.kind == A52CTL_STICK) [self.owner stickDeflect:0 y:0 port:c.port];
    else [self.owner ctlPressed:c down:NO];
}

/* The owner let go (hide, rebuild): the mouse no longer holds anything. */
- (void)forgetMouse
{
    _down = nil;
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
    A52ConsoleView *_console;
    NSArray<A52Ctl *> *_ctls;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives controller N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_helpButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_mouseLabel;
    NSPopUpButton *_mousePort;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _consoleW, _consoleH;
    int _heldTarget;                        /* what the mouse holds here, or -1 */
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
    _heldTarget = -1;
    _padGeneration = (unsigned)-1;

    _panel = [[A52ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 480, 640)
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

    _content = [[A52CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 480, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[A52ConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.mapTarget = -1;
    _console.showKeys = a52session_get_int(_session, "show_bindings", 0) != 0;
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the controller it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[i]];
        _padEff[i] = [NSTextField labelWithString:@""];
        [_padEff[i] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[i] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[i]];
        /* -1 = automatic (connection order), 0-3 = that controller */
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Controller 1", @"Controller 2",
                                            @"Controller 3", @"Controller 4"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[i]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _helpButton = A52HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show keys" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];

    _mouseLabel = [NSTextField labelWithString:@"Mouse in the picture drives"];
    [_content addSubview:_mouseLabel];
    _mousePort = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_mousePort addItemsWithTitles:@[@"Controller 1", @"Controller 2", @"Controller 3", @"Controller 4"]];
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

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the keypad and the sticks ---------------------------------------------------- */

- (A52Ctl *)ctlWithFace:(NSString *)face rect:(NSRect)rect target:(int)target kind:(int)kind
{
    A52Ctl *c = [[A52Ctl alloc] init];
    c.face = face;
    c.rect = rect;
    c.target = target;
    c.kind = kind;
    return c;
}

/* Points from key units, rounded so neighbouring caps keep an even gap. */
static CGFloat snap(CGFloat v)
{
    return floor(v + 0.5);
}

/* One controller's stick and its two fire buttons in its body. */
- (void)addStick:(int)port box:(NSRect)sb to:(NSMutableArray<A52Ctl *> *)ctls
{
    const CGFloat cx = sb.origin.x + 78, cy = sb.origin.y + 104;
    A52Ctl *stick = [self ctlWithFace:@"" rect:NSMakeRect(cx - STICK_R, cy - STICK_R, 2 * STICK_R, 2 * STICK_R)
                               target:A52_TARGET_PORT(port, A52_ACT_UP) kind:A52CTL_STICK];
    stick.port = port;
    [ctls addObject:stick];
    [ctls addObject:[self ctlWithFace:@"FIRE" rect:NSMakeRect(sb.origin.x + 150, sb.origin.y + 50, 54, 50)
                               target:A52_TARGET_PORT(port, A52_ACT_FIRE) kind:A52CTL_FIRE]];
    [ctls addObject:[self ctlWithFace:@"FIRE 2" rect:NSMakeRect(sb.origin.x + 150, sb.origin.y + 108, 54, 50)
                               target:A52_TARGET_PORT(port, A52_ACT_FIRE2) kind:A52CTL_FIRE2]];
}

/* The console's drawing area and everything on it. */
- (void)buildConsole
{
    int n = 0;
    float kw = 0, kh = 0;
    const a52_keycap *caps = a52session_keyboard_layout(&n, &kw, &kh);
    CGFloat unit = KBD_UNIT_MAX;
    if (kw > 0 && (CGFloat)kw * unit > KBD_WIDTH_MAX) unit = KBD_WIDTH_MAX / (CGFloat)kw;

    const CGFloat kbdW = snap((CGFloat)kw * unit), kbdH = snap((CGFloat)kh * unit);
    const CGFloat plateW = kbdW + 2 * PLATE_PAD, plateH = kbdH + 2 * PLATE_PAD;
    const CGFloat sticksW = 2 * STICK_W + STICK_GAP;
    const CGFloat sticksH = 2 * STICK_H + STICK_GAP;
    /* the keypad on the left, the four controllers in a block on the right */
    const NSRect plate = NSMakeRect(MARGIN, HEADER_H, plateW, plateH);
    _consoleW = MARGIN + plateW + SECTION_GAP + sticksW + MARGIN;

    NSMutableArray<A52Ctl *> *ctls = [NSMutableArray array];
    for (int i = 0; caps && i < n; i++) {
        const a52_keycap *k = &caps[i];
        const CGFloat x0 = snap((CGFloat)k->x * unit), y0 = snap((CGFloat)k->y * unit);
        const CGFloat x1 = snap((CGFloat)(k->x + k->w) * unit), y1 = snap((CGFloat)(k->y + k->h) * unit);
        [ctls addObject:[self ctlWithFace:nsstr(k->label)
                                     rect:NSMakeRect(plate.origin.x + PLATE_PAD + x0,
                                                     plate.origin.y + PLATE_PAD + y0, x1 - x0, y1 - y0)
                                   target:A52_TARGET_KEY(k->key) kind:k->kind]];
    }

    const CGFloat sticksY = HEADER_H;
    const CGFloat sticksX = MARGIN + plateW + SECTION_GAP;
    NSMutableArray<NSValue *> *boxes = [NSMutableArray array];
    for (int port = 0; port < A52_PORTS; port++) {
        const NSRect sb = NSMakeRect(sticksX + (port % 2) * (STICK_W + STICK_GAP),
                                     sticksY + (port / 2) * (STICK_H + STICK_GAP), STICK_W, STICK_H);
        [boxes addObject:[NSValue valueWithRect:sb]];
        [self addStick:port box:sb to:ctls];
    }
    _consoleH = HEADER_H + MAX(plateH, sticksH) + MARGIN;

    _ctls = ctls;
    _console.controls = ctls;
    _console.stickBoxes = boxes;
    _console.plate = plate;
    _console.title = @"ATARI 5200";
}

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
    const CGFloat w = _consoleW;
    const CGFloat inner = w - 2 * MARGIN;
    const CGFloat padsY = _consoleH + MARGIN;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 8;
    const CGFloat mouseY = mapY + BTN_H + 8;
    const CGFloat hintY = mouseY + 28;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(w, total)];
    [_console setFrame:NSMakeRect(0, 0, w, _consoleH)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat y = rowsY + i * ROW_H;
        [_padName[i] setFrame:NSMakeRect(MARGIN, y + 4, inner - POPUP_W - EFF_W - 16, 18)];
        [_padEff[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 8, y + 4, EFF_W, 18)];
        [_padAssign[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, y, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 76, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 82, mapY, 96, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 190, mapY + 3, 110, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(MARGIN + inner - _helpButton.frame.size.width, mapY + 2)];
    [_mouseLabel setFrame:NSMakeRect(MARGIN, mouseY + 3, 190, 18)];
    [_mousePort setFrame:NSMakeRect(MARGIN + 196, mouseY - 1, POPUP_W + 10, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_console setNeedsDisplay:YES];
}

- (void)rebuild
{
    [self letGo];
    [self buildConsole];
    [self relayout];
    [self refreshLabels];
    [self refreshMousePort];
}

- (void)refreshMousePort
{
    int p = a52session_get_int(_session, "mouse_port", 0);
    if (p < 0 || p >= A52_PORTS) p = 0;
    [_mousePort selectItemAtIndex:p];
}

- (void)mousePortChanged:(NSPopUpButton *)sender
{
    a52session_set_int(_session, "mouse_port", (int)sender.indexOfSelectedItem);
}

/* ---- pressing --------------------------------------------------------------------- */

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console forgetMouse];
    for (int port = 0; port < A52_PORTS; port++)
        a52session_stick_set(_session, port, 0, 0);
    if (_heldTarget >= 0) {
        a52session_press(_session, _heldTarget, 0);
        _heldTarget = -1;
    }
}

/* The mouse holds the stick at (x, y): it follows the pointer. */
- (void)stickDeflect:(int)x y:(int)y port:(int)port
{
    if (_mapState != -2) return;
    a52session_stick_set(_session, port, x, y);
    [self pollHeld];
}

/* Map: a stick direction was clicked. */
- (void)stickPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)ctlPressed:(A52Ctl *)c down:(BOOL)down
{
    if (_mapState != -2) {
        /* armed: picks (or changes) the control to rebind */
        if (down) [self setMapState:c.target];
        return;
    }
    if (down) {
        [self letGo];
        _heldTarget = c.target;
        a52session_press(_session, c.target, 1);
    } else if (_heldTarget == c.target) {
        _heldTarget = -1;
        a52session_press(_session, c.target, 0);
    }
    [self pollHeld];
}

/* What the console sees held -- host keyboard, gamepads and this panel -- lit. */
- (void)pollHeld
{
    if (_mapState != -2) return;    /* Map shows what is bound, not what is held */
    unsigned held[A52_PORTS];
    for (int port = 0; port < A52_PORTS; port++) held[port] = a52session_buttons_held(_session, port);
    const unsigned keys = a52session_keys_held(_session);
    for (A52Ctl *c in _ctls) {
        const int t = c.target;
        if (c.kind == A52CTL_STICK) {
            int ax = 0, ay = 0;
            a52session_stick_get(_session, c.port, &ax, &ay);
            const unsigned m = held[c.port] & ((1u << A52_ACT_FIRE) - 1u);
            if (m != c.mask || ax != c.ax || ay != c.ay) {
                c.mask = m;
                c.ax = ax;
                c.ay = ay;
                [_console setNeedsDisplayInRect:NSInsetRect(c.rect, -4, -4)];
            }
            continue;
        }
        BOOL on;
        if (t < A52_PORTS * A52_ACT_PER_PORT)
            on = ((held[t / A52_ACT_PER_PORT] >> (t % A52_ACT_PER_PORT)) & 1u) != 0;
        else
            on = ((keys >> (t - A52_TARGET_KEY(0))) & 1u) != 0;
        on = on || t == _heldTarget;
        if (c.lit != on) {
            c.lit = on;
            [_console setNeedsDisplayInRect:c.rect];
        }
    }
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(a52session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(int)target
{
    const a52_binding b = a52session_binding_get(_session, target);
    NSString *key = keyName(b.keysym);
    NSString *text = key.length ? key : @"no key";
    if (b.button != A52_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@, pad %@", text, nsstr(a52_pad_button_name(b.button))];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    const BOOL mapping = _mapState != -2;
    _console.mapping = mapping;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    if (mapping)
        for (A52Ctl *c in _ctls) { c.lit = NO; c.mask = 0; c.ax = c.ay = 0; }
    [_console setNeedsDisplay:YES];
    [self pollHeld];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        a52session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(a52_target_name(state)), [self bindingText:state]];
    } else {
        a52session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1 ? @"Click a key, a stick direction or a fire button to remap"
                                        : @"Click and hold to press a key; drag a stick.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : rgb(A52SESSION_ACCENT_RGB);
    [self refreshLabels];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(a52_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"%@ is now %@ (it was %@)", what, name, nsstr(stolen)]
        : [NSString stringWithFormat:@"%@ is now %@", what, name];
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

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a52session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL on = [_showKeysBox state] == NSControlStateValueOn;
    a52session_set_int(_session, "show_bindings", on ? 1 : 0);
    _console.showKeys = on;
    [_console setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int i = 0; i < _padRows; i++) {
        const int eff = a52session_gamepad_effective_port(_session, i);
        NSString *text = eff >= 0 ? [NSString stringWithFormat:@"drives controller %d", eff + 1] : @"drives nothing";
        if (![_padEff[i].stringValue isEqualToString:text]) _padEff[i].stringValue = text;
    }
}

- (void)refreshPads
{
    const int n = a52session_gamepad_count(_session);
    _padGeneration = a52session_gamepad_generation(_session);
    _padRows = n < MAX_PAD_ROWS ? (n > 0 ? n : 0) : MAX_PAD_ROWS;
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < _padRows;
        _padName[i].hidden = !shown;
        _padEff[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a52session_gamepad_name(_session, i, name, (int)sizeof name);
        _padName[i].stringValue = nsstr(name);
        _padName[i].toolTip = nsstr(name);
        const int assign = a52session_gamepad_assignment(_session, i);
        [_padAssign[i] selectItemAtIndex:(assign < 0 || assign >= A52_PORTS ? 0 : assign + 1)];
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
 * every 250 ms the hot-plugged pads and auto-assignment. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (a52session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
    }
    [self pollHeld];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak A52ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
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
        /* the next key is the binding; Esc backs out of the pick */
        if (ks == A52_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:ks swallow:YES];
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
        if (ks && down) {
            [self bindKey:ks swallow:NO];
            return;
        }
    }
    A52ForwardFlags(_session, e);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self refreshPads];
    [self refreshMousePort];
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

- (void)refresh
{
    if ([self isVisible]) {
        [self refreshPads];
        [self refreshLabels];
    }
}

/* Only what this panel's mouse holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
