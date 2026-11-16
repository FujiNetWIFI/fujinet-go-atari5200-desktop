/*
 * DisplayExtras.inc -- the mouse as the analog stick, and the screenshot and
 * frame-count hooks, included into the display view's @implementation (see
 * DisplayGlue.h).
 *
 * The mouse: dragging in the picture moves the analog stick of the controller
 * the "mouse_port" setting names (0-3, the Controllers window's choice) from
 * where the drag began; the right button is that controller's FIRE.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* A mouse drag deflects the stick fully at this fraction of the picture's
 * height from where it began. */
#define DRAG_FRACTION 0.2

- (void)extraInit {}

- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

/* Frames the machine has run (A52_AUTOQUIT_FRAMES). */
- (uint64_t)frames { return atomic_load(&_frameCount); }

/* The current frame as a PNG, taken from the session, so it works while the
 * frame clock is stopped. */
- (BOOL)savePng:(NSString *)path
{
    const size_t bytes = (size_t)A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT * 4;
    uint32_t *px = calloc(1, bytes);
    if (!px) return NO;
    int w = 0, h = 0, par = 1000;
    uint64_t serial = 0;    /* 0 forces a copy */
    BOOL ok = NO;
    if (a52session_copy_frame(_session, px, &w, &h, &par, &serial) && w > 0 && h > 0) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, (size_t)w * h * 4, NULL);
        if (dp) {
            CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                           (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                           dp, NULL, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(dp);
            if (img) {
                NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
                NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                ok = png && [png writeToFile:path atomically:YES];
                CGImageRelease(img);
            }
        }
        CGColorSpaceRelease(cs);
    }
    free(px);
    return ok;
}

/* ---- the mouse: the analog stick ----------------------------------------------- */

- (int)mousePort
{
    const int p = a52session_get_int(_session, "mouse_port", 0);
    return p >= 0 && p < A52_PORTS ? p : 0;
}

- (void)dragTo:(NSEvent *)e
{
    if (!_dragging) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    const double radius = fmax(24.0, DRAG_FRACTION * [self pictureRect].size.height);
    /* AppKit counts up from the bottom; the stick's + is down */
    double dx = (p.x - _dragStart.x) / radius, dy = -(p.y - _dragStart.y) / radius;
    dx = fmax(-1.0, fmin(1.0, dx));
    dy = fmax(-1.0, fmin(1.0, dy));
    a52session_stick_set(_session, _dragPort, (int)lround(dx * 127.0), (int)lround(dy * 127.0));
}

- (void)mouseDown:(NSEvent *)e
{
    _dragging = YES;
    _dragPort = [self mousePort];
    _dragStart = [self convertPoint:[e locationInWindow] fromView:nil];
    [self dragTo:e];
}

- (void)mouseDragged:(NSEvent *)e { [self dragTo:e]; }

- (void)endDrag
{
    if (_dragging) a52session_stick_set(_session, _dragPort, 0, 0);
    _dragging = NO;
    if (_fireHeld) a52session_press(_session, A52_TARGET_PORT(_dragPort, A52_ACT_FIRE), 0);
    _fireHeld = NO;
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (_dragging) a52session_stick_set(_session, _dragPort, 0, 0);
    _dragging = NO;
}

- (void)rightMouseDown:(NSEvent *)e
{
    (void)e;
    if (_fireHeld) return;
    _fireHeld = YES;
    _dragPort = [self mousePort];
    a52session_press(_session, A52_TARGET_PORT(_dragPort, A52_ACT_FIRE), 1);
}

- (void)rightMouseUp:(NSEvent *)e
{
    (void)e;
    if (!_fireHeld) return;
    _fireHeld = NO;
    a52session_press(_session, A52_TARGET_PORT(_dragPort, A52_ACT_FIRE), 0);
}
