//
//  overlay.mm
//  vanta-base
//
//  A native drawing surface for the Drawing library.
//
//  Drawing is specified as an overlay the game cannot see. Rendering it with
//  GuiObjects under gethui() is visible to anything that walks CoreGui, which
//  is exactly what a compliance suite checks for - and more to the point, it is
//  not what the library is supposed to be. So the shapes are drawn into a
//  borderless, click-through NSWindow that floats over Roblox's own window and
//  mirrors its frame.
//
//  This deliberately does not hook the renderer. The macOS reference draws
//  through ImGui by interposing on CAMetalLayer's nextDrawable and
//  MTLCommandBuffer's commit; that puts our code on the render thread's hot
//  path, and a mistake there takes the client down mid-frame. A separate window
//  costs a compositor pass and cannot corrupt anything Roblox owns.
//
//  Everything here is written for manual retain/release - the file is compiled
//  without ARC, like the rest of the Objective-C in this project.

#import <Cocoa/Cocoa.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Overlay.h"

namespace overlay {

namespace {

    using ShapeList = std::vector<Shape>;

    std::mutex g_lock;
    std::unordered_map<uint64_t, Shape> g_shapes;

    // Draw order is by ZIndex and then by creation. The sorted list is built
    // lazily: rebuilding it inside `set` meant a script animating N shapes paid
    // N sorts of N elements every frame, and the answer was thrown away again
    // before anything looked at it.
    std::shared_ptr<const ShapeList> g_ordered;

    // Bumped by anything that changes what should be on screen. The timer that
    // drives redraws compares it against what it last painted, so a client with
    // no Drawing in it - which is nearly all of them - never repaints at all.
    // None of the four references that implement Drawing pays for a redraw
    // either: they all draw inside a frame the game was already rendering
    // (mac hooks MTLCommandBuffer's commit, sourcessss and Vicna hook Present).
    // A window of our own cannot do that, so the least it can do is stay still.
    std::atomic<uint64_t> g_generation{0};

    // Whether anything is visible at all. With nothing to show the window is
    // ordered out, which is the same thing mac's `NotifyGameState(false)` does
    // to its overlay when there is no game.
    std::atomic<bool> g_anyVisible{false};

    std::atomic<bool> g_started{false};
    std::atomic<bool> g_usable{false};

    // Called with g_lock held.
    void invalidateLocked()
    {
        g_ordered.reset();
        bool visible = false;
        for (const auto& entry : g_shapes) {
            if (entry.second.visible && entry.second.transparency < 1.0) {
                visible = true;
                break;
            }
        }
        g_anyVisible.store(visible, std::memory_order_release);
        g_generation.fetch_add(1, std::memory_order_release);
    }

    std::shared_ptr<const ShapeList> snapshot()
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_ordered)
            return g_ordered;

        auto ordered = std::make_shared<ShapeList>();
        ordered->reserve(g_shapes.size());
        for (const auto& entry : g_shapes)
            ordered->push_back(entry.second);
        std::stable_sort(ordered->begin(), ordered->end(),
                         [](const Shape& a, const Shape& b) {
                             if (a.zindex != b.zindex)
                                 return a.zindex < b.zindex;
                             return a.id < b.id;
                         });
        g_ordered = ordered;
        return g_ordered;
    }

}  // namespace

// What `follow` needs to know from the shape list, without reaching into the
// anonymous namespace above from Objective-C.
uint64_t generation() { return g_generation.load(std::memory_order_acquire); }
bool anythingVisible() { return g_anyVisible.load(std::memory_order_acquire); }

}  // namespace overlay


// The view that does the drawing. Flipped, so a Drawing's coordinates mean the
// same thing they do in Roblox: origin top-left, y growing downwards.
@interface VantaOverlayView : NSView
@end

@implementation VantaOverlayView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return NO; }
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }

- (NSFont*)fontForId:(int)identifier size:(CGFloat)size
{
    switch (identifier) {
        case 1:  // System
            return [NSFont systemFontOfSize:size];
        case 2:  // Plex
            return [NSFont fontWithName:@"IBM Plex Sans" size:size]
                       ?: [NSFont systemFontOfSize:size];
        case 3:  // Monospace
            return [NSFont userFixedPitchFontOfSize:size]
                       ?: [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];
        default:  // UI
            return [NSFont systemFontOfSize:size];
    }
}

- (void)strokeOrFillPath:(CGContextRef)context shape:(const overlay::Shape&)shape
{
    if (shape.filled)
        CGContextFillPath(context);
    else {
        CGContextSetLineWidth(context, shape.thickness > 0 ? shape.thickness : 1.0);
        CGContextStrokePath(context);
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    if (!context)
        return;

    CGContextClearRect(context, self.bounds);

    const std::shared_ptr<const overlay::ShapeList> shapes = overlay::snapshot();
    for (const overlay::Shape& shape : *shapes) {
        if (!shape.visible)
            continue;

        // Transparency is the spec's: 0 opaque, 1 invisible.
        const CGFloat alpha = 1.0 - std::min(std::max(shape.transparency, 0.0), 1.0);
        if (alpha <= 0.0)
            continue;

        CGContextSetRGBStrokeColor(context, shape.r, shape.g, shape.b, alpha);
        CGContextSetRGBFillColor(context, shape.r, shape.g, shape.b, alpha);

        switch (shape.kind) {
            case overlay::Kind::Line: {
                CGContextBeginPath(context);
                CGContextMoveToPoint(context, shape.points[0].x, shape.points[0].y);
                CGContextAddLineToPoint(context, shape.points[1].x, shape.points[1].y);
                CGContextSetLineWidth(context, shape.thickness > 0 ? shape.thickness : 1.0);
                CGContextStrokePath(context);
                break;
            }
            case overlay::Kind::Square: {
                const CGRect rect = CGRectMake(shape.points[0].x, shape.points[0].y,
                                               shape.size.x, shape.size.y);
                CGContextBeginPath(context);
                CGContextAddRect(context, rect);
                [self strokeOrFillPath:context shape:shape];
                break;
            }
            case overlay::Kind::Circle: {
                CGContextBeginPath(context);
                if (shape.numsides >= 3) {
                    // NumSides means what it says: a polygon, not a circle.
                    for (int i = 0; i < shape.numsides; i++) {
                        const double angle = (2.0 * M_PI * i) / shape.numsides;
                        const CGFloat x = shape.points[0].x + std::cos(angle) * shape.radius;
                        const CGFloat y = shape.points[0].y + std::sin(angle) * shape.radius;
                        if (i == 0)
                            CGContextMoveToPoint(context, x, y);
                        else
                            CGContextAddLineToPoint(context, x, y);
                    }
                    CGContextClosePath(context);
                } else {
                    CGContextAddEllipseInRect(
                        context, CGRectMake(shape.points[0].x - shape.radius,
                                            shape.points[0].y - shape.radius,
                                            shape.radius * 2, shape.radius * 2));
                }
                [self strokeOrFillPath:context shape:shape];
                break;
            }
            case overlay::Kind::Triangle:
            case overlay::Kind::Quad: {
                const int count = shape.kind == overlay::Kind::Triangle ? 3 : 4;
                CGContextBeginPath(context);
                CGContextMoveToPoint(context, shape.points[0].x, shape.points[0].y);
                for (int i = 1; i < count; i++)
                    CGContextAddLineToPoint(context, shape.points[i].x, shape.points[i].y);
                CGContextClosePath(context);
                [self strokeOrFillPath:context shape:shape];
                break;
            }
            case overlay::Kind::Image: {
                if (shape.data.empty())
                    break;
                NSData* bytes = [NSData dataWithBytes:shape.data.data()
                                               length:shape.data.size()];
                NSImage* image = [[[NSImage alloc] initWithData:bytes] autorelease];
                if (!image)
                    break;
                const CGRect rect = CGRectMake(shape.points[0].x, shape.points[0].y,
                                               shape.size.x, shape.size.y);
                CGContextSaveGState(context);
                CGContextSetAlpha(context, alpha);
                [image drawInRect:NSRectFromCGRect(rect)
                         fromRect:NSZeroRect
                        operation:NSCompositingOperationSourceOver
                         fraction:1.0];
                CGContextRestoreGState(context);
                break;
            }
            case overlay::Kind::Text: {
                if (shape.text.empty())
                    break;
                NSString* value = [NSString stringWithUTF8String:shape.text.c_str()];
                if (!value)
                    break;

                NSFont* font = [self fontForId:shape.font size:shape.textsize];
                NSColor* colour = [NSColor colorWithSRGBRed:shape.r
                                                      green:shape.g
                                                       blue:shape.b
                                                      alpha:alpha];
                NSMutableDictionary* attributes = [NSMutableDictionary dictionary];
                attributes[NSFontAttributeName] = font;
                attributes[NSForegroundColorAttributeName] = colour;
                if (shape.outline) {
                    // A negative stroke width means "stroke and fill", which is
                    // what an outlined label wants.
                    attributes[NSStrokeWidthAttributeName] = @(-3.0);
                    attributes[NSStrokeColorAttributeName] =
                        [NSColor colorWithSRGBRed:shape.outline_r
                                            green:shape.outline_g
                                             blue:shape.outline_b
                                            alpha:alpha];
                }

                const NSSize measured = [value sizeWithAttributes:attributes];
                CGFloat x = shape.points[0].x;
                if (shape.center)
                    x -= measured.width / 2.0;
                [value drawAtPoint:NSMakePoint(x, shape.points[0].y)
                    withAttributes:attributes];
                break;
            }
        }
    }
}

@end

@interface VantaOverlayController : NSObject
@property(nonatomic, retain) NSWindow* window;
@property(nonatomic, retain) VantaOverlayView* view;
@property(nonatomic, assign) uint64_t painted;
- (void)follow;
@end

@implementation VantaOverlayController

// The window Roblox is actually playing in: the biggest visible one it owns
// that is big enough to be a viewport.
//
// The size floor matters. Roblox keeps small utility windows around, and early
// in a session `[NSApp windows]` can hand back one with a zero frame - which
// the old "biggest wins" rule happily picked, leaving the overlay sized 0x0 and
// drawing nowhere. Anything under 200x150 is not the game.
//
// keyWindow alone is not enough either: it is nil whenever the client is not
// frontmost, and the overlay should keep tracking either way. It is used as a
// tie-break, not as the answer.
- (NSWindow*)gameWindow
{
    NSWindow* best = nil;
    CGFloat bestArea = 0;
    NSWindow* key = [NSApp keyWindow];

    for (NSWindow* candidate in [NSApp windows]) {
        if (candidate == self.window || !candidate.isVisible)
            continue;
        const NSRect frame = candidate.frame;
        if (frame.size.width < 200 || frame.size.height < 150)
            continue;
        CGFloat area = frame.size.width * frame.size.height;
        if (candidate == key)
            area *= 1.5;            // the focused window wins a close call
        if (area > bestArea) {
            bestArea = area;
            best = candidate;
        }
    }
    return best;
}

- (void)follow
{
    NSWindow* game = [self gameWindow];

    // The content rect, not the frame: a Drawing's (0,0) is the top-left of the
    // game's viewport, not of its title bar.
    //
    // When there is no game window to follow the overlay covers the main screen
    // instead of hiding. Roblox does not always put its window in
    // `[NSApp windows]` - a relaunched client here reported none at all - and
    // the old behaviour then left the overlay sized 0x0, which looks exactly
    // like the library being broken. A full-screen overlay is still the right
    // surface to draw on; it just is not glued to anything.
    NSRect content;
    if (game) {
        content = [game contentRectForFrameRect:game.frame];
    } else {
        NSScreen* screen = [NSScreen mainScreen];
        if (!screen) {
            if (self.window.isVisible)
                [self.window orderOut:nil];
            return;
        }
        content = screen.frame;
    }

    if (content.size.width < 1 || content.size.height < 1)
        return;

    // The frame is tracked whether or not anything is being drawn, so a script
    // that creates its first Drawing gets an overlay that is already the right
    // size rather than one that snaps into place a frame later.
    BOOL moved = !NSEqualRects(self.window.frame, content);
    if (moved)
        [self.window setFrame:content display:NO];

    // Nothing to show: take the window down. A borderless transparent window
    // that is up costs a compositor pass per frame for as long as it is there,
    // and nearly every client never draws anything at all.
    if (!overlay::anythingVisible()) {
        if (self.window.isVisible)
            [self.window orderOut:nil];
        return;
    }

    if (!self.window.isVisible) {
        [self.window orderFrontRegardless];
        moved = YES;
    }

    // The redraw is the expensive half - a full-window clear plus a backing
    // store update - and it used to happen thirty times a second whether or not
    // a single pixel had changed. None of the four references that implement
    // Drawing pays for a redraw at all: they draw inside a frame the game was
    // already rendering. The next best thing is to repaint only when the shapes
    // actually moved.
    const uint64_t generation = overlay::generation();
    if (!moved && generation == self.painted)
        return;
    self.painted = generation;
    [self.view setNeedsDisplay:YES];
}

@end

namespace overlay {

namespace {
    VantaOverlayController* g_controller = nil;

    void buildOnMain()
    {
        @autoreleasepool {
            if (g_controller)
                return;

            VantaOverlayController* controller = [[VantaOverlayController alloc] init];

            NSWindow* window =
                [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                            styleMask:NSWindowStyleMaskBorderless
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
            window.opaque = NO;
            window.backgroundColor = [NSColor clearColor];
            window.hasShadow = NO;
            window.ignoresMouseEvents = YES;
            window.level = NSStatusWindowLevel;
            window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                        NSWindowCollectionBehaviorFullScreenAuxiliary |
                                        NSWindowCollectionBehaviorStationary |
                                        NSWindowCollectionBehaviorIgnoresCycle;
            // Never take focus away from the game.
            [window setReleasedWhenClosed:NO];

            VantaOverlayView* view =
                [[VantaOverlayView alloc] initWithFrame:window.contentView.bounds];
            view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            window.contentView = view;

            controller.window = window;
            controller.view = view;
            [window release];
            [view release];

            g_controller = controller;
            g_usable.store(true, std::memory_order_release);

            // One timer keeps the overlay glued to the game window; it is also
            // what redraws, so a change made from the Luau thread shows up
            // without that thread ever touching AppKit.
            [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                            repeats:YES
                                              block:^(NSTimer* timer) {
                                                (void)timer;
                                                [g_controller follow];
                                              }];
            [g_controller follow];
        }
    }
}  // namespace

void start()
{
    if (g_started.exchange(true))
        return;

    if ([NSThread isMainThread]) {
        buildOnMain();
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      buildOnMain();
    });
}

bool available()
{
    start();
    if (g_usable.load(std::memory_order_acquire))
        return true;

    // start() bounced the window's construction to the main queue, so the
    // first caller would otherwise be told "no" before AppKit had run at all.
    // Roblox's main thread is the AppKit thread and is always being serviced,
    // so a short wait here settles it; failing that we really do fall back.
    for (int attempt = 0; attempt < 100 && !g_usable.load(std::memory_order_acquire); attempt++)
        usleep(5 * 1000);
    return g_usable.load(std::memory_order_acquire);
}

void set(const Shape& shape)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_shapes[shape.id] = shape;
    invalidateLocked();
}

void remove(uint64_t id)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_shapes.erase(id);
    invalidateLocked();
}

void clear()
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_shapes.clear();
    invalidateLocked();
}

void textBounds(const std::string& text, double size, int font, double* width, double* height)
{
    *width = 0;
    *height = 0;
    @autoreleasepool {
        NSString* value = [NSString stringWithUTF8String:text.c_str()];
        if (!value)
            return;
        NSFont* resolved = nil;
        switch (font) {
            case 1: resolved = [NSFont systemFontOfSize:size]; break;
            case 2: resolved = [NSFont fontWithName:@"IBM Plex Sans" size:size]
                                   ?: [NSFont systemFontOfSize:size]; break;
            case 3: resolved = [NSFont userFixedPitchFontOfSize:size]; break;
            default: resolved = [NSFont systemFontOfSize:size]; break;
        }
        if (!resolved)
            resolved = [NSFont systemFontOfSize:size];
        const NSSize measured =
            [value sizeWithAttributes:@{ NSFontAttributeName : resolved }];
        *width = measured.width;
        *height = measured.height;
    }
}

Snapshot inspect()
{
    Snapshot out;
    if (!g_controller)
        return out;

    __block Snapshot result;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        result.shapes = static_cast<int>(g_shapes.size());
    }

    void (^work)(void) = ^{
      @autoreleasepool {
          VantaOverlayController* controller = g_controller;
          if (!controller || !controller.view)
              return;
          result.built = true;

          const NSRect frame = controller.window.frame;
          result.x = frame.origin.x;
          result.y = frame.origin.y;
          result.w = frame.size.width;
          result.h = frame.size.height;

          NSWindow* game = [controller gameWindow];
          if (game) {
              const NSRect content = [game contentRectForFrameRect:game.frame];
              result.gamex = content.origin.x;
              result.gamey = content.origin.y;
              result.gamew = content.size.width;
              result.gameh = content.size.height;
          }

          NSView* view = controller.view;
          NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
          if (!rep)
              return;
          [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];

          const NSInteger width = rep.pixelsWide;
          const NSInteger height = rep.pixelsHigh;
          long painted = 0;
          double sr = 0, sg = 0, sb = 0;
          // A coarse sample is enough to answer "did anything get painted";
          // walking every pixel of a 4K window from a probe is not.
          for (NSInteger y = 0; y < height; y += 4) {
              for (NSInteger x = 0; x < width; x += 4) {
                  NSColor* pixel = [rep colorAtX:x y:y];
                  if (!pixel || pixel.alphaComponent <= 0.01)
                      continue;
                  painted++;
                  NSColor* rgb = [pixel colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
                  if (rgb) {
                      sr += rgb.redComponent;
                      sg += rgb.greenComponent;
                      sb += rgb.blueComponent;
                  }
              }
          }
          result.painted = painted;
          if (painted > 0) {
              result.r = sr / painted;
              result.g = sg / painted;
              result.b = sb / painted;
          }
      }
    };

    if ([NSThread isMainThread])
        work();
    else
        dispatch_sync(dispatch_get_main_queue(), work);
    return result;
}


}  // namespace overlay
