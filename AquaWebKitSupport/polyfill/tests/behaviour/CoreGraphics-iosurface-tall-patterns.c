// The CGContextDrawTiledImage replacement and the fill, stroke and text hooks (polyfills/c/CoreGraphics.c) for tiles
// and pattern cells tall enough that 10.9's software CoreAnimation renderer, stepping texture coordinates per
// destination row in single precision, would draw them out of place. A 16000-row tile repeated over 16000 rows --
// tiled, as an image pattern and as a drawn pattern cell -- lands every row in its place, also under a 512-row clip.
// A tall tile under a shadow, a tall tile at a device scale of 1.5, and an image pattern turned a quarter land within
// 40 levels of alpha of CoreGraphics' own bitmap draw. A rect filled with a tall pattern under the copy and clear blend
// modes leaves the pixels outside it as they were, on the surface and inside a transparency layer. A drawn cell
// 10,000,000 rows tall, which no band of rows places, fills as 10.9 fills it. Under the software renderer text drawn
// with a tall pattern in a clipping text mode draws and then clips. Runs under the machine's renderer and again under
// the software-renderer interposer built from CoreGraphics-iosurface-row-placement.c.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo);
extern CGPatternRef CGPatternCreateWithImage2(CGImageRef, CGAffineTransform, CGPatternTiling);

static const CGBitmapInfo kBGRA = (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
enum { kWide = 8 };

static int failures;
static void check(int ok, const char *what)
{
    printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
    if (!ok)
        failures++;
}

static IOSurfaceRef createSurface(int width, int height)
{
    int bytes = 4;
    unsigned format = 'BGRA';
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberIntType, &width);
    CFNumberRef h = CFNumberCreate(NULL, kCFNumberIntType, &height);
    CFNumberRef b = CFNumberCreate(NULL, kCFNumberIntType, &bytes);
    CFNumberRef f = CFNumberCreate(NULL, kCFNumberIntType, &format);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
    const void *values[] = { w, h, b, f };
    CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    CFRelease(f);
    CFRelease(b);
    CFRelease(h);
    CFRelease(w);
    return surface;
}

static void releasePixels(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

// A width x height image whose row y holds blue y & 0xff, green y >> 8, red 77, opaque.
static CGImageRef createRowsImage(CGColorSpaceRef space, int width, int height)
{
    uint8_t *pixels = malloc((size_t)width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *p = pixels + ((size_t)y * width + x) * 4;
            p[0] = (uint8_t)(y & 0xff);
            p[1] = (uint8_t)(y >> 8);
            p[2] = 77;
            p[3] = 255;
        }
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)width * height * 4, releasePixels);
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    return image;
}

static void drawPatternCell(void *info, CGContextRef context)
{
    CGImageRef image = info;
    CGContextDrawImage(context, CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image)), image);
}

static void drawRedCell(void *info, CGContextRef context)
{
    (void)info;
    CGContextSetRGBFillColor(context, 1, 0, 0, 1);
    CGContextFillRect(context, CGRectMake(0, 0, 8, 100));
}

static void setFillPattern(CGContextRef context, CGPatternRef pattern)
{
    CGColorSpaceRef patternSpace = CGColorSpaceCreatePattern(NULL);
    CGContextSetFillColorSpace(context, patternSpace);
    CGColorSpaceRelease(patternSpace);
    CGFloat alpha = 1;
    CGContextSetFillPattern(context, pattern, &alpha);
}

typedef void (*Draw)(CGContextRef, void *);

// Pixels of a width x height surface drawn by `draw` whose alpha differs from a CoreGraphics bitmap drawn the same
// way by more than 40.
static size_t pixelsOffBitmap(CGColorSpaceRef space, int width, int height, Draw draw, void *info)
{
    IOSurfaceRef surface = createSurface(width, height);
    CGContextRef context = CGIOSurfaceContextCreate(surface, width, height, 8, 32, space, kBGRA);
    CGContextRef reference = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space, kBGRA);
    CGContextClearRect(context, CGRectMake(0, 0, width, height));
    draw(context, info);
    draw(reference, info);
    CGContextFlush(context);
    size_t off = 0;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
        const uint8_t *referenceRow = (const uint8_t *)CGBitmapContextGetData(reference) + (size_t)y * CGBitmapContextGetBytesPerRow(reference);
        for (int x = 0; x < width; x++)
            off += abs((int)row[x * 4 + 3] - (int)referenceRow[x * 4 + 3]) > 40;
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    CGContextRelease(reference);
    CGContextRelease(context);
    CFRelease(surface);
    return off;
}

// A 16x16000 tile under a shadow 300 device pixels to the side and 2000 up, in a 400x16000 surface.
static void drawShadowedTile(CGContextRef context, void *info)
{
    CGColorRef black = CGColorCreateGenericGray(0, 1);
    CGContextSetShadowWithColor(context, CGSizeMake(300, 2000), 2, black);
    CGColorRelease(black);
    CGContextDrawTiledImage(context, CGRectMake(0, 0, 16, 16000), (CGImageRef)info);
}

// A 16x16000 tile at a device scale of 1.5, so its rows and its tile seams fall between device pixels.
static void drawScaledTile(CGContextRef context, void *info)
{
    CGContextScaleCTM(context, 1.5, 1.5);
    CGContextDrawTiledImage(context, CGRectMake(0, 0, 16, 16000), (CGImageRef)info);
}

// An image pattern of a 16000x100 image turned a quarter, filling a 120x16000 surface.
static void drawTurnedPattern(CGContextRef context, void *info)
{
    CGPatternRef pattern = CGPatternCreateWithImage2((CGImageRef)info, CGAffineTransformMake(0, 1, -1, 0, 100, 0), kCGPatternTilingConstantSpacing);
    setFillPattern(context, pattern);
    CGContextFillRect(context, CGRectMake(0, 0, 120, 16000));
    CGPatternRelease(pattern);
}

int main(void)
{
    // The renderer QuartzCore draws IOSurface contexts with, chosen as CA::CG::IOSurfaceRenderer::acquire chooses it.
    typedef int (*ChooseFn)(const int *, void **, int *);
    typedef int (*CreateFn)(void *, void *, void **);
    void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    ChooseFn choose = gl ? (ChooseFn)dlsym(gl, "CGLChoosePixelFormat") : NULL;
    CreateFn create = gl ? (CreateFn)dlsym(gl, "CGLCreateContext") : NULL;
    static const int attributes[] = { 8, 32, 73, 72, 96, 1262, 0 };
    void *pixelFormat = NULL, *glContext = NULL;
    int formats = 0;
    bool accelerated = choose && create && !choose(attributes, &pixelFormat, &formats) && pixelFormat && formats > 0
        && !create(pixelFormat, NULL, &glContext) && glContext;
    printf("    QuartzCore renderer: %s\n", accelerated ? "accelerated" : "software");
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef tall = createRowsImage(space, kWide, 16000);

    for (int way = 0; way < 6; way++) {
        bool clipped = way >= 3;
        IOSurfaceRef surface = createSurface(kWide, 16000);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, 16000, 8, 32, space, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, kWide, 16000));
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
        if (clipped)
            CGContextClipToRect(context, CGRectMake(0, 8000, kWide, 512));
        CGPatternRef pattern = NULL;
        if (way % 3 == 0)
            CGContextDrawTiledImage(context, CGRectMake(0, 0, kWide, 16000), tall);
        else {
            static const CGPatternCallbacks callbacks = { 0, drawPatternCell, NULL };
            pattern = way % 3 == 1 ? CGPatternCreateWithImage2(tall, CGAffineTransformIdentity, kCGPatternTilingConstantSpacing)
                : CGPatternCreate(tall, CGRectMake(0, 0, kWide, 16000), CGAffineTransformIdentity, kWide, 16000, kCGPatternTilingConstantSpacing, true,
                    &callbacks);
            setFillPattern(context, pattern);
            CGContextFillRect(context, CGRectMake(0, 0, kWide, 16000));
        }
        CGContextFlush(context);
        size_t wrong = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        for (int r = clipped ? 16000 - 8512 : 0; r < (clipped ? 16000 - 8000 : 16000); r++) {
            const uint8_t *q = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)r * IOSurfaceGetBytesPerRow(surface);
            wrong += (q[0] | q[1] << 8) != r || q[2] != 77;
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        static const char *const ways[] = { "tiled image", "image pattern", "drawn pattern" };
        char what[160];
        snprintf(what, sizeof what, "%s of a 16000-row tile over 16000 rows%s: %zu rows out of place", ways[way % 3],
            clipped ? " under a 512-row clip" : "", wrong);
        check(!wrong, what);
        if (pattern)
            CGPatternRelease(pattern);
        CGContextRelease(context);
        CFRelease(surface);
    }

    CGImageRef wideTile = createRowsImage(space, 16, 16000);
    char what[160];
    size_t off = pixelsOffBitmap(space, 400, 16000, drawShadowedTile, (void *)wideTile);
    snprintf(what, sizeof what, "16x16000 tile under a shadow: %zu pixels off the bitmap's", off);
    check(!off, what);
    off = pixelsOffBitmap(space, 400, 16000, drawScaledTile, (void *)wideTile);
    snprintf(what, sizeof what, "16x16000 tile at a device scale of 1.5: %zu pixels off the bitmap's", off);
    check(!off, what);
    CGImageRef turnedImage = createRowsImage(space, 16000, 100);
    off = pixelsOffBitmap(space, 120, 16000, drawTurnedPattern, (void *)turnedImage);
    snprintf(what, sizeof what, "16000x100 image pattern turned a quarter: %zu pixels off the bitmap's", off);
    check(!off, what);
    CGImageRelease(turnedImage);
    CGImageRelease(wideTile);

    // Copy and clear fill only their rect: the rest of a magenta surface, or of a magenta transparency layer, stays magenta.
    static const CGBlendMode modes[] = { kCGBlendModeCopy, kCGBlendModeClear };
    for (int m = 0; m < 4; m++) {
        bool layer = m >= 2;
        IOSurfaceRef surface = createSurface(64, 16000);
        CGContextRef context = CGIOSurfaceContextCreate(surface, 64, 16000, 8, 32, space, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, 64, 16000));
        if (layer)
            CGContextBeginTransparencyLayer(context, NULL);
        CGContextSetRGBFillColor(context, 1, 0, 1, 1);
        CGContextFillRect(context, CGRectMake(0, 0, 64, 16000));
        CGPatternRef pattern = CGPatternCreateWithImage2(tall, CGAffineTransformIdentity, kCGPatternTilingConstantSpacing);
        setFillPattern(context, pattern);
        CGContextSetBlendMode(context, modes[m % 2]);
        CGContextFillRect(context, CGRectMake(16, 0, 16, 16000));
        if (layer)
            CGContextEndTransparencyLayer(context);
        CGContextFlush(context);
        size_t changed = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        for (int y = 0; y < 16000; y++) {
            const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
            for (int x = 0; x < 64; x++) {
                if (x >= 16 && x < 32)
                    continue;
                changed += !(row[x * 4] == 255 && row[x * 4 + 1] == 0 && row[x * 4 + 2] == 255 && row[x * 4 + 3] == 255);
            }
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        snprintf(what, sizeof what, "tall-pattern rect filled under %s%s: %zu pixels outside it changed", m % 2 ? "clear" : "copy",
            layer ? " in a transparency layer" : "", changed);
        check(!changed, what);
        CGPatternRelease(pattern);
        CGContextRelease(context);
        CFRelease(surface);
    }

    // A drawn cell 10,000,000 device rows tall, as WebCore's repeat-x pattern makes one under a large scale, which no
    // band of rows places, fills a surface as 10.9 fills it: every pixel red.
    {
        IOSurfaceRef surface = createSurface(64, 512);
        CGContextRef context = CGIOSurfaceContextCreate(surface, 64, 512, 8, 32, space, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, 64, 512));
        static const CGPatternCallbacks callbacks = { 0, drawRedCell, NULL };
        CGPatternRef pattern = CGPatternCreate(NULL, CGRectMake(0, 0, 8, 100), CGAffineTransformMakeScale(1, 1e5), 8, 1 << 22,
            kCGPatternTilingConstantSpacing, true, &callbacks);
        setFillPattern(context, pattern);
        CGContextFillRect(context, CGRectMake(0, 0, 64, 512));
        CGContextFlush(context);
        size_t red = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        for (int y = 0; y < 512; y++) {
            const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
            for (int x = 0; x < 64; x++)
                red += row[x * 4 + 2] > 200 && row[x * 4 + 3] > 200;
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        snprintf(what, sizeof what, "rect filled with a drawn cell 10,000,000 rows tall: %zu of 32768 pixels red", red);
        check(red == 32768, what);
        CGPatternRelease(pattern);
        CGContextRelease(context);
        CFRelease(surface);
    }

    // Text in a clipping mode, filled with a tall pattern, which the software renderer draws in bands, then clips to the
    // glyphs: a fill after it leaves the far corner clear. (10.9's accelerated renderer paints no text in a clipping
    // mode into an IOSurface, whatever its fill.)
    {
        IOSurfaceRef surface = createSurface(200, 16000);
        CGContextRef context = CGIOSurfaceContextCreate(surface, 200, 16000, 8, 32, space, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, 200, 16000));
        CGPatternRef pattern = CGPatternCreateWithImage2(tall, CGAffineTransformIdentity, kCGPatternTilingConstantSpacing);
        setFillPattern(context, pattern);
        CGContextSelectFont(context, "Helvetica", 40, kCGEncodingMacRoman);
        CGContextSetTextDrawingMode(context, kCGTextFillClip);
        CGContextShowTextAtPoint(context, 10, 100, "Tall", 4);
        CGContextFlush(context);
        size_t painted = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        for (int y = 0; y < 16000; y++) {
            const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
            for (int x = 0; x < 200; x++)
                painted += row[x * 4 + 3] > 0;
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        snprintf(what, sizeof what, "text filled with a tall pattern in a clipping mode draws: %zu pixels painted", painted);
        check(accelerated || painted > 100, what);
        CGContextSetRGBFillColor(context, 0, 0, 1, 1);
        CGContextFillRect(context, CGRectMake(0, 0, 200, 16000));
        CGContextFlush(context);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        uint8_t corner = ((const uint8_t *)IOSurfaceGetBaseAddress(surface))[190 * 4 + 3];
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        snprintf(what, sizeof what, "then clips to the glyphs: alpha %d in the far corner after a fill", corner);
        check(accelerated || !corner, what);
        CGPatternRelease(pattern);
        CGContextRelease(context);
        CFRelease(surface);
    }

    CGImageRelease(tall);
    CGColorSpaceRelease(space);
    return failures ? 1 : 0;
}
