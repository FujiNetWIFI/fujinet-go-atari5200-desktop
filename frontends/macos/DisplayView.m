/*
 * A52DisplayView -- the framebuffer, as a Core Animation layer fed from a
 * CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * The energy rules this view follows, as the other frontends do:
 *
 *   - It is LAYER-BACKED and has no drawRect:. The picture is a CGImage set
 *     as the contents of a sublayer, which the compositor scales (nearest
 *     neighbour, or linear for Smooth) on the GPU.
 *   - The display link runs ONLY while the window can be seen: on screen, not
 *     occluded, not miniaturized, the app not hidden. The machine itself
 *     keeps running and paces on the wall clock until the link returns.
 *   - A tick redraws only when the session published a new frame (the serial
 *     a52session_copy_frame keeps): a static CONFIG screen does no image, no
 *     dispatch and no commit, whatever the link's rate.
 *   - Pixels go into a small POOL of reused buffers. Each frame's CGImage is
 *     backed by a CGDataProvider over one pool buffer, and the provider's
 *     release callback returns the buffer to the pool when Core Animation has
 *     let go of the image.
 *
 * The frame's size follows the visible area setting (336 x 224 NTSC by
 * default); its pixel aspect (x1000) says how wide each pixel is, and the TV
 * aspect gives the Atari's pixels their shape on a TV.
 *
 * The mouse: dragging in the picture moves the analog stick of the controller
 * the "mouse_port" setting names (0-3, the Controllers window's choice) from
 * where the drag began; the right button is that controller's FIRE.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Buffers in flight: one being filled, one waiting for the main thread, one
 * or two held by the compositor. */
#define POOL_SIZE 4

#define FRAME_BYTES ((size_t)A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT * 4)

/* A mouse drag deflects the stick fully at this fraction of the picture's
 * height from where it began. */
#define DRAG_FRACTION 0.2

/* One frame's pixels. `busy` (guarded by @synchronized on the class) is set
 * while the buffer is filling, queued, or referenced by an image. */
@interface A52FrameBuffer : NSObject
@property (nonatomic, readonly) uint32_t *pixels;
@property (nonatomic) BOOL busy;
@end

@implementation A52FrameBuffer
- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _pixels = calloc(1, FRAME_BYTES);
    return self;
}

- (void)dealloc
{
    free(_pixels);
}
@end

/* The image's data provider lets go: the buffer is free again. */
static void releaseFrame(void *info, const void *data, size_t size)
{
    (void)data;
    (void)size;
    A52FrameBuffer *buf = (__bridge_transfer A52FrameBuffer *)info;
    @synchronized ([A52FrameBuffer class]) {
        buf.busy = NO;
    }
}

@implementation A52DisplayView {
    a52session *_session;
    CVDisplayLinkRef _link;          /* NULL until first wanted; Start/Stop after */
    BOOL _linkRunning;               /* main thread */
    NSArray<A52FrameBuffer *> *_pool;
    uint64_t _serial;                /* the link thread's: the last frame copied */
    CGColorSpaceRef _cs;
    CALayer *_picture;
    BOOL _tv;
    BOOL _smooth;
    int _w, _h, _par;                /* the picture shown (main thread) */
    NSLock *_lock;                   /* guards _pending, _pw/_ph/_ppar and _posted */
    CGImageRef _pending;             /* the newest image the main thread has not shown */
    int _pw, _ph, _ppar;
    BOOL _posted;                    /* a main-thread block is queued to show it */
    /* the mouse */
    BOOL _dragging;
    int _dragPort;
    NSPoint _dragStart;
    BOOL _fireHeld;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    A52DisplayView *self = (__bridge A52DisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(a52session *)session
{
    /* 336 x 224 at three times, a TV's shape */
    self = [super initWithFrame:NSMakeRect(0, 0, 864, 720)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;
    _w = 336;
    _h = 224;
    _par = 1000;
    _lock = [[NSLock alloc] init];
    _cs = CGColorSpaceCreateDeviceRGB();

    NSMutableArray<A52FrameBuffer *> *pool = [NSMutableArray array];
    for (int i = 0; i < POOL_SIZE; i++) [pool addObject:[[A52FrameBuffer alloc] init]];
    _pool = pool;

    [self setWantsLayer:YES];
    _picture = [CALayer layer];
    _picture.contentsGravity = kCAGravityResize;
    _picture.magnificationFilter = kCAFilterNearest;
    _picture.minificationFilter = kCAFilterLinear;
    _picture.opaque = YES;
    [[self layer] addSublayer:_picture];
    [self positionPicture];
    return self;
}

/* The root layer: black, the letterbox. */
- (CALayer *)makeBackingLayer
{
    CALayer *root = [CALayer layer];
    root.backgroundColor = [[NSColor blackColor] CGColor];
    return root;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)dealloc
{
    [self stop];
    if (_cs) CGColorSpaceRelease(_cs);
}

- (uint64_t)frames
{
    return a52session_frame_count(_session);
}

/* ---- the picture's place ---------------------------------------------------- */

- (double)pictureAspect
{
    const double w = _w * _par / 1000.0, h = _h;
    if (h <= 0) return 4.0 / 3.0;
    if (!_tv) return w / h;
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a52session_refresh_rate(_session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

- (void)positionPicture
{
    if (!_picture) return;
    const NSRect b = [self bounds];
    const double want = [self pictureAspect];
    const double w = b.size.width, h = b.size.height;
    if (w <= 0 || h <= 0) return;
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.frame = CGRectMake(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh);
    [CATransaction commit];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self positionPicture];
}

/* AppKit does not promise setFrame: goes through setFrameSize:. */
- (void)setFrame:(NSRect)frame
{
    [super setFrame:frame];
    [self positionPicture];
}

- (void)setTvAspect:(BOOL)tv
{
    _tv = tv;
    [self positionPicture];
}

- (void)setSmooth:(BOOL)smooth
{
    _smooth = smooth;
    _picture.magnificationFilter = smooth ? kCAFilterLinear : kCAFilterNearest;
}

/* ---- the frame clock: only while the window can be seen --------------------- */

- (BOOL)shouldRun
{
    NSWindow *w = [self window];
    if (!w || ![w isVisible] || [w isMiniaturized]) return NO;
    if (!([w occlusionState] & NSWindowOcclusionStateVisible)) return NO;
    if ([NSApp isHidden]) return NO;
    return YES;
}

/* The panel the window is on: the link follows it, so its ticks come at that
 * panel's refresh. */
- (void)retargetLink
{
    NSScreen *screen = [[self window] screen];
    NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (num && _link) CVDisplayLinkSetCurrentCGDisplay(_link, (CGDirectDisplayID)[num unsignedIntValue]);
}

- (void)startLink
{
    if (!_link) {
        if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) != kCVReturnSuccess) {
            _link = NULL;
            return;
        }
        CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    }
    [self retargetLink];
    if (!_linkRunning) {
        CVDisplayLinkStart(_link);
        _linkRunning = YES;
    }
}

- (void)stopLink
{
    if (_link && _linkRunning) CVDisplayLinkStop(_link);
    _linkRunning = NO;
}

- (void)updateVisibility
{
    if ([self shouldRun]) [self startLink];
    else [self stopLink];
}

- (void)visibilityChanged:(NSNotification *)note
{
    /* a closing window is still "visible" while the notice is delivered */
    if ([[note name] isEqualToString:NSWindowWillCloseNotification]) {
        [self stopLink];
        return;
    }
    [self updateVisibility];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:self];
    NSWindow *w = [self window];
    if (w) {
        NSArray<NSString *> *names = @[
            NSWindowDidChangeOcclusionStateNotification,
            NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification,
            NSWindowDidChangeScreenNotification,
            NSWindowWillCloseNotification,
        ];
        for (NSString *name in names)
            [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:w];
        /* Cmd+H hides every window without minimising them */
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidHideNotification object:nil];
        [nc addObserver:self selector:@selector(visibilityChanged:)
                   name:NSApplicationDidUnhideNotification object:nil];
    }
    [self updateVisibility];
}

/* Final teardown: the link, the observers, any frame still waiting. */
- (void)stop
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopLink];
    if (_link) {
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
    [_lock lock];
    CGImageRef old = _pending;
    _pending = NULL;
    [_lock unlock];
    if (old) CGImageRelease(old);
}

/* ---- the link's thread -------------------------------------------------------- */

/* A free buffer, marked busy, or nil when the compositor still holds them all
 * (the tick is then skipped: the frame is copied on the next one). */
- (A52FrameBuffer *)takeBuffer
{
    @synchronized ([A52FrameBuffer class]) {
        for (A52FrameBuffer *b in _pool) {
            if (!b.busy) {
                b.busy = YES;
                return b;
            }
        }
    }
    return nil;
}

/* The image over a buffer's pixels (+1), w x h packed. The provider keeps
 * the buffer alive and frees it for reuse when the image's last owner lets
 * go. */
- (CGImageRef)newImageFor:(A52FrameBuffer *)buf width:(int)w height:(int)h
{
    void *info = (void *)CFBridgingRetain(buf);
    CGDataProviderRef dp = CGDataProviderCreateWithData(info, [buf pixels], (size_t)w * h * 4, releaseFrame);
    if (!dp) {
        CFRelease(info);     /* the caller frees the buffer's slot */
        return NULL;
    }
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit image with the alpha byte skipped reads them as they are. */
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, _cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    return img;
}

- (void)tick:(uint64_t)hostTime
{
    @autoreleasepool {
        static double toNs = 0.0;
        if (toNs == 0.0) {
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            toNs = (double)tb.numer / (double)tb.denom;
        }
        a52session_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

        /* Only a new frame is worth a buffer, an image and a trip to the main
         * thread: copy_frame says nothing changed by returning 0. */
        A52FrameBuffer *buf = [self takeBuffer];
        if (!buf) return;
        int w = 0, h = 0, par = 1000;
        if (!a52session_copy_frame(_session, [buf pixels], &w, &h, &par, &_serial) || w <= 0 || h <= 0) {
            @synchronized ([A52FrameBuffer class]) {
                buf.busy = NO;
            }
            return;
        }
        CGImageRef img = [self newImageFor:buf width:w height:h];
        if (!img) {
            @synchronized ([A52FrameBuffer class]) {
                buf.busy = NO;
            }
            return;
        }

        /* The newest frame wins: one still waiting for the main thread is
         * dropped, and only one block is ever queued. */
        [_lock lock];
        CGImageRef old = _pending;
        _pending = img;
        _pw = w;
        _ph = h;
        _ppar = par > 0 ? par : 1000;
        const BOOL post = !_posted;
        _posted = YES;
        [_lock unlock];
        if (old) CGImageRelease(old);
        if (post) dispatch_async(dispatch_get_main_queue(), ^{ [self present]; });
    }
}

/* ---- the main thread ------------------------------------------------------------ */

- (void)present
{
    [_lock lock];
    CGImageRef img = _pending;
    _pending = NULL;
    _posted = NO;
    const int w = _pw, h = _ph, par = _ppar;
    [_lock unlock];
    if (!img) return;
    const BOOL reshape = w != _w || h != _h || par != _par;
    _w = w;
    _h = h;
    _par = par;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
    if (reshape) [self positionPicture];
}

- (BOOL)savePng:(NSString *)path
{
    uint32_t *px = calloc(1, FRAME_BYTES);
    if (!px) return NO;
    int w = 0, h = 0, par = 1000;
    uint64_t serial = 0;    /* 0 forces a copy */
    BOOL ok = NO;
    if (a52session_copy_frame(_session, px, &w, &h, &par, &serial) && w > 0 && h > 0) {
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, (size_t)w * h * 4, NULL);
        if (dp) {
            CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, _cs,
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
    }
    free(px);
    return ok;
}

/* ---- the mouse: the analog stick -------------------------------------------------- */

- (int)mousePort
{
    const int p = a52session_get_int(_session, "mouse_port", 0);
    return p >= 0 && p < A52_PORTS ? p : 0;
}

- (void)dragTo:(NSEvent *)e
{
    if (!_dragging) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    const double radius = fmax(24.0, DRAG_FRACTION * _picture.frame.size.height);
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
@end
