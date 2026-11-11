// The CGContextDrawImage replacement (polyfills/c/CoreGraphics.c) for an image of an IOSurface context drawn
// into an IOSurface context of its own colour space. A draw mapping the image one to one onto whole device
// pixels is made from a copy of the part its clip shows, taken before the call returns: overwriting the
// image's surface before the destination renders leaves the draw unchanged, and it lands, byte for byte, what
// 10.9's own definition lands drawing the whole image. The cases: a rectangular clip with the image partly off
// the surface, one at fractional bounds, a path clip, a clip along the image's edge, an alpha below one, a
// multiply and a copy blend, and an image 20000 rows tall. A draw under a shadow, a
// flipped draw and one at a half-pixel offset draw the whole image, and land what 10.9 lands. The parts copied
// are released with their draws.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo);
CGImageRef CGIOSurfaceContextCreateImage(CGContextRef);
typedef void (*DrawImageFn)(CGContextRef, CGRect, CGImageRef);

static const CGBitmapInfo kBGRA = (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
enum { kImageWidth = 600, kImageHeight = 900, kSurfaceWidth = 640, kSurfaceHeight = 480 };

static int failures;
static void check(int ok, const char *what)
{
    printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
    if (!ok)
        failures++;
}

static DrawImageFn systemDrawImage(void)
{
    const struct mach_header *image = NSAddImage("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
        NSADDIMAGE_OPTION_RETURN_ON_ERROR);
    NSSymbol found = image ? NSLookupSymbolInImage(image, "_CGContextDrawImage", NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR) : 0;
    return found ? (DrawImageFn)NSAddressOfSymbol(found) : 0;
}

// This process's live IOSurfaces: 10.9 gives a process's surfaces IDs from 1 up, reusing freed ones, and
// IOSurfaceLookup finds a surface of this process by its ID.
static long surfaceCount(void)
{
    long count = 0;
    for (IOSurfaceID surfaceID = 1; surfaceID < 65536; surfaceID++) {
        IOSurfaceRef surface = IOSurfaceLookup(surfaceID);
        if (surface) {
            count++;
            CFRelease(surface);
        }
    }
    return count;
}

static IOSurfaceRef createSurface(size_t width, size_t height)
{
    int w = (int)width, h = (int)height, bytesPerElement = 4;
    unsigned format = 'BGRA';
    CFNumberRef widthNumber = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef heightNumber = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef element = CFNumberCreate(NULL, kCFNumberIntType, &bytesPerElement);
    CFNumberRef pixelFormat = CFNumberCreate(NULL, kCFNumberIntType, &format);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
    const void *values[] = { widthNumber, heightNumber, element, pixelFormat };
    CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    CFRelease(widthNumber);
    CFRelease(heightNumber);
    CFRelease(element);
    CFRelease(pixelFormat);
    return surface;
}

// Translucent premultiplied pixels of varied colour, or an opaque backdrop, written by the CPU.
static void fillSurface(IOSurfaceRef surface, size_t width, size_t height, bool backdrop)
{
    IOSurfaceLock(surface, 0, NULL);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            unsigned char *p = (unsigned char *)IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface) + x * 4;
            unsigned alpha = backdrop ? 255 : 30 + (x * 7 + y * 13) % 226;
            unsigned colour[3] = { (x * 31 + y * 3) % 256, (x * 5 + y * 17) % 256, backdrop ? 200 : (x * y) % 256 };
            for (int c = 0; c < 3; c++)
                p[2 - c] = (unsigned char)(colour[c] * alpha / 255);
            p[3] = (unsigned char)alpha;
        }
    }
    IOSurfaceUnlock(surface, 0, NULL);
}

enum { kRectClip, kFractionalClip, kPathClip, kEdgeClip, kAlpha, kMultiply, kCopy, kShadow, kFlipped, kHalfPixel, kCases };
static const int firstWholeImageCase = kShadow;
static const char *const caseNames[kCases] = {
    "a rectangular clip, the image partly off the surface", "a rectangular clip at fractional bounds", "a path clip",
    "a clip along the image's edge", "an alpha of one half", "a multiply blend", "a copy blend",
    "a shadow with an offset and a blur", "a flipped draw", "a half-pixel offset",
};

static void drawCase(int which, CGContextRef context, DrawImageFn draw, CGImageRef image)
{
    CGRect rect = CGRectMake(-50, -300, kImageWidth, kImageHeight);
    switch (which) {
    case kRectClip:
        CGContextClipToRect(context, CGRectMake(37, 23, 261, 147));
        break;
    case kPathClip:
        CGContextAddEllipseInRect(context, CGRectMake(80.5, 60.25, 300, 200));
        CGContextClip(context);
        break;
    case kFractionalClip:
        CGContextClipToRect(context, CGRectMake(37.5, 23.25, 261, 147.5));
        break;
    case kEdgeClip:
        CGContextClipToRect(context, CGRectMake(-50, 100, 120, 200));
        break;
    case kCopy:
        CGContextClipToRect(context, CGRectMake(120, 200, 160, 90));
        CGContextSetBlendMode(context, kCGBlendModeCopy);
        break;
    case kShadow: {
        CGContextClipToRect(context, CGRectMake(150, 150, 120, 90));
        CGColorRef shadow = CGColorCreateGenericRGB(0.1, 0.2, 0.7, 0.8);
        CGContextSetShadowWithColor(context, CGSizeMake(7, -5), 6, shadow);
        CGColorRelease(shadow);
        break;
    }
    case kAlpha:
        CGContextClipToRect(context, CGRectMake(200, 40, 150, 300));
        CGContextSetAlpha(context, 0.5);
        break;
    case kMultiply:
        CGContextClipToRect(context, CGRectMake(10, 300, 400, 120));
        CGContextSetBlendMode(context, kCGBlendModeMultiply);
        break;
    case kFlipped:
        CGContextClipToRect(context, CGRectMake(37, 23, 261, 147));
        CGContextTranslateCTM(context, 0, kSurfaceHeight);
        CGContextScaleCTM(context, 1, -1);
        break;
    case kHalfPixel:
        CGContextClipToRect(context, CGRectMake(37, 23, 261, 147));
        CGContextTranslateCTM(context, 0.5, 0);
        break;
    }
    draw(context, rect, image);
}

static void archiveDraw(CGContextRef context, CGRect rect, CGImageRef image)
{
    CGContextDrawImage(context, rect, image);
}

int main(void)
{
    DrawImageFn system = systemDrawImage();
    CGColorSpaceRef sRGB = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // The renderer QuartzCore draws IOSurface contexts with, chosen as CA::CG::IOSurfaceRenderer::acquire chooses it.
    bool accelerated = false;
    {
        typedef int (*ChooseFn)(const int *, void **, int *);
        typedef int (*CreateFn)(void *, void *, void **);
        void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
        ChooseFn choose = gl ? (ChooseFn)dlsym(gl, "CGLChoosePixelFormat") : NULL;
        CreateFn create = gl ? (CreateFn)dlsym(gl, "CGLCreateContext") : NULL;
        static const int attributes[] = { 8, 32, 73, 72, 96, 1262, 0 };
        void *pixelFormat = NULL, *glContext = NULL;
        int formats = 0;
        accelerated = choose && create && !choose(attributes, &pixelFormat, &formats) && pixelFormat && formats > 0
            && !create(pixelFormat, NULL, &glContext) && glContext;
        printf("    QuartzCore renderer: %s\n", accelerated ? "accelerated" : "software");
    }
    if (!system || !sRGB)
        return 1;
    IOSurfaceRef source = createSurface(kImageWidth, kImageHeight);
    fillSurface(source, kImageWidth, kImageHeight, false);
    CGContextRef sourceContext = CGIOSurfaceContextCreate(source, kImageWidth, kImageHeight, 8, 32, sRGB, kBGRA);

    for (int which = 0; which < kCases; which++) {
        // side 0: 10.9's draw, rendered at once. side 1: the archive's draw, rendered after the image's surface
        // is overwritten. side 2: the archive's draw, rendered at once.
        unsigned char *results[3];
        for (int side = 0; side < 3; side++) {
            CGImageRef image = CGIOSurfaceContextCreateImage(sourceContext);
            IOSurfaceRef surface = createSurface(kSurfaceWidth, kSurfaceHeight);
            fillSurface(surface, kSurfaceWidth, kSurfaceHeight, true);
            CGContextRef context = CGIOSurfaceContextCreate(surface, kSurfaceWidth, kSurfaceHeight, 8, 32, sRGB, kBGRA);
            CGContextSaveGState(context);
            drawCase(which, context, side ? archiveDraw : system, image);
            CGContextRestoreGState(context);
            CGImageRelease(image);
            if (side == 1) {
                IOSurfaceLock(source, 0, NULL);
                memset(IOSurfaceGetBaseAddress(source), 0x80, IOSurfaceGetAllocSize(source));
                IOSurfaceUnlock(source, 0, NULL);
            }
            CGContextFlush(context);
            if (side == 1)
                fillSurface(source, kImageWidth, kImageHeight, false);
            results[side] = malloc(kSurfaceWidth * kSurfaceHeight * 4);
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            for (size_t y = 0; y < kSurfaceHeight; y++)
                memcpy(results[side] + y * kSurfaceWidth * 4, (unsigned char *)IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface), kSurfaceWidth * 4);
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            CGContextRelease(context);
            CFRelease(surface);
        }
        size_t atOnce = 0, overwritten = 0;
        for (size_t i = 0; i < kSurfaceWidth * kSurfaceHeight * 4; i++) {
            atOnce += results[0][i] != results[2][i];
            overwritten += results[0][i] != results[1][i];
        }
        char what[200];
        snprintf(what, sizeof what, "%s: byte-identical to 10.9's whole draw", caseNames[which]);
        if (atOnce)
            printf("    %zu bytes differ\n", atOnce);
        check(!atOnce, what);
        if (which < firstWholeImageCase)
            snprintf(what, sizeof what, "  and drawn from a copy of its part (%zu bytes changed by the overwrite)", overwritten);
        else
            snprintf(what, sizeof what, "  and drawn from the whole image (%zu bytes changed by the overwrite)", overwritten);
        check(which < firstWholeImageCase ? !overwritten : overwritten > 0, what);
        free(results[0]);
        free(results[1]);
        free(results[2]);
    }
    CGContextRelease(sourceContext);
    CFRelease(source);

    // An image 20000 rows tall, over the software renderer's texture limit, each row encoding its number, drawn
    // one to one showing 4 rows and 16000, within the tallest surface the accelerated renderer draws into: the
    // destination holds exactly those image rows, and they survive the image's surface being overwritten before
    // the destination renders.
    {
        enum { kWide = 8, kTall = 20000, kFirst = 2500 };
        IOSurfaceRef tall = createSurface(kWide, kTall);
        CGContextRef tallContext = CGIOSurfaceContextCreate(tall, kWide, kTall, 8, 32, sRGB, kBGRA);
        static const size_t shownRows[] = { 4, 16000 };
        for (size_t s = 0; s < 2; s++) {
            size_t shown = shownRows[s];
            IOSurfaceLock(tall, 0, NULL);
            for (size_t y = 0; y < kTall; y++) {
                for (size_t x = 0; x < kWide; x++) {
                    unsigned char *p = (unsigned char *)IOSurfaceGetBaseAddress(tall) + y * IOSurfaceGetBytesPerRow(tall) + x * 4;
                    p[0] = (unsigned char)(y & 0xff);
                    p[1] = (unsigned char)(y >> 8);
                    p[2] = 77;
                    p[3] = 255;
                }
            }
            IOSurfaceUnlock(tall, 0, NULL);
            CGImageRef image = CGIOSurfaceContextCreateImage(tallContext);
            IOSurfaceRef surface = createSurface(kWide, shown);
            CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, shown, 8, 32, sRGB, kBGRA);
            CGContextClearRect(context, CGRectMake(0, 0, kWide, shown));
            // Image row kFirst + r lies along destination row r counted from the top.
            CGContextDrawImage(context, CGRectMake(0, (CGFloat)shown + kFirst - kTall, kWide, kTall), image);
            CGImageRelease(image);
            IOSurfaceLock(tall, 0, NULL);
            memset(IOSurfaceGetBaseAddress(tall), 0x80, IOSurfaceGetAllocSize(tall));
            IOSurfaceUnlock(tall, 0, NULL);
            CGContextFlush(context);
            size_t wrongRows = 0;
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            for (size_t r = 0; r < shown; r++) {
                const unsigned char *p = (const unsigned char *)IOSurfaceGetBaseAddress(surface) + r * IOSurfaceGetBytesPerRow(surface);
                size_t row = kFirst + r;
                bool exact = true;
                for (size_t x = 0; x < kWide; x++)
                    exact &= p[x * 4] == (row & 0xff) && p[x * 4 + 1] == (row >> 8) && p[x * 4 + 2] == 77 && p[x * 4 + 3] == 255;
                wrongRows += !exact;
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            char what[160];
            printf("    20000-row image, %zu rows shown: %zu rows wrong\n", shown, wrongRows);
            snprintf(what, sizeof what, "an image 20000 rows tall showing %zu rows lands exactly those rows from a copy", shown);
            check(!wrongRows, what);
            CGContextRelease(context);
            CFRelease(surface);
        }
        CGContextRelease(tallContext);
        CFRelease(tall);
    }

    // 2000 one-to-one draws, each copying a part into a surface of its own, leave no surfaces behind once their
    // destinations are rendered and released: once QuartzCore has let go of what it renders, this process holds
    // fewer than 10 more IOSurfaces than before them.
    {
        IOSurfaceRef source = createSurface(kImageWidth, kImageHeight);
        fillSurface(source, kImageWidth, kImageHeight, false);
        CGContextRef sourceContext = CGIOSurfaceContextCreate(source, kImageWidth, kImageHeight, 8, 32, sRGB, kBGRA);
        long before = surfaceCount();
        for (int i = 0; i < 2000; i++) {
            CGImageRef image = CGIOSurfaceContextCreateImage(sourceContext);
            IOSurfaceRef surface = createSurface(64, 64);
            CGContextRef context = CGIOSurfaceContextCreate(surface, 64, 64, 8, 32, sRGB, kBGRA);
            CGContextClipToRect(context, CGRectMake(5, 7, 40, 30));
            CGContextDrawImage(context, CGRectMake(-(i % 300), -(i % 500), kImageWidth, kImageHeight), image);
            CGImageRelease(image);
            CGContextFlush(context);
            CGContextRelease(context);
            CFRelease(surface);
        }
        long after = surfaceCount();
        for (int wait = 0; wait < 30; wait++) {
            usleep(100000);
            long now = surfaceCount();
            if (now >= after)
                break;
            after = now;
        }
        printf("    IOSurface objects before %ld, after 2000 draws %ld\n", before, after);
        check(before >= 0 && after >= 0 && after - before < 10, "parts copied for one-to-one draws are released with their draws");
        CGContextRelease(sourceContext);
        CFRelease(source);
    }

    CGColorSpaceRelease(sRGB);
    return failures ? 1 : 0;
}
