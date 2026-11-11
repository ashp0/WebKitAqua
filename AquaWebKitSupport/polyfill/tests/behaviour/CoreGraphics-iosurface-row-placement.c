// The CGContextDrawImage replacement (polyfills/c/CoreGraphics.c) for tall and oversized images in IOSurface
// contexts. 10.9's software CoreAnimation renderer draws nothing of an image wider or taller than 16384 and steps
// texture coordinates per destination row in single precision; its accelerated renderer leaves up to a few columns or
// rows unpainted at the top and right edges of an image over the GPU's texture limit drawn scaled, turned or at a
// fractional offset. Images whose rows each encode their number -- from memory, from an IOSurface context, under an
// image mask, without interpolation, and in grey and alpha -- drawn one to one into IOSurfaces up to 16000 rows tall
// at several offsets land every row in its place, also under a shadow beside them, and under one thousands of rows
// away within a time budget. A 9021-row image drawn into 512-row surfaces at every tile position lands every row in
// its place. A 9021-row image of an IOSurface context paints every row of every 512-row tile when drawn as a page
// tile draws it: flipped, at a device scale of 2, into a surface of another colour space, through the image of
// matched rows. Images 40000 tall, 40000 wide and 20000 tall shrunk into smaller surfaces paint every pixel, also
// when the 40000-row one is drawn in two passes, each clipped to part of a surface of another colour space.
// Translucent images and opaque ones under a context alpha of one half, 20000, 40000 and 100000 rows tall, shrunk to
// several heights and also turned a quarter, paint every pixel exactly once. Turned 10, 30 and 45 degrees, shrunk to
// a few pixels, shrunk evenly from 17000 square, shrunk under a shadow 150 pixels away, or with a top or right edge
// in view enlarged, one to one at a half-pixel origin, or turned 5 degrees, tall and oversized images land within 40
// levels of alpha of CoreGraphics' own bitmap draw of them. 10.9's own draw of 512- and 1024-row windows of a
// 16000-row image lands in place unaided. The probe runs under the renderer the machine has and again under the
// software renderer, which built with WK_PROBE_SOFTWARE_RENDERER it forces by failing CGLChoosePixelFormat, as
// CA::CG::IOSurfaceRenderer::acquire meets it on a Mac without an accelerated renderer.
#ifdef WK_PROBE_SOFTWARE_RENDERER
#include <OpenGL/OpenGL.h>
#include <stddef.h>

static CGLError failChoosePixelFormat(const CGLPixelFormatAttribute *attributes, CGLPixelFormatObj *pixelFormat, GLint *count)
{
    (void)attributes;
    *pixelFormat = NULL;
    if (count)
        *count = 0;
    return kCGLBadPixelFormat;
}

__attribute__((used)) static const struct {
    const void *replacement;
    const void *replaced;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)failChoosePixelFormat, (const void *)CGLChoosePixelFormat },
};
#else
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>

CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo);
CGImageRef CGIOSurfaceContextCreateImage(CGContextRef);

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

// Row y of every image holds blue y & 0xff, green y >> 8, red 77, opaque.
static void fillRows(uint8_t *base, size_t bytesPerRow, size_t height)
{
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < kWide; x++) {
            uint8_t *p = base + y * bytesPerRow + x * 4;
            p[0] = (uint8_t)(y & 0xff);
            p[1] = (uint8_t)(y >> 8);
            p[2] = 77;
            p[3] = 255;
        }
    }
}

static void releaseRows(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

enum ImageKind { kMemory, kSurface, kMasked, kNotInterpolating, kGrayAlpha };
static const char *const kKindNames[] = { "memory", "surface", "masked", "non-interpolating", "grey+alpha" };

// A retained image `height` rows tall of `kind`: from memory, of an IOSurface context whose surface keeps `*keep`,
// from memory under an image mask that paints every pixel, from memory without interpolation, or 8-bit grey and
// alpha holding grey y & 0xff in row y.
static CGImageRef createImage(CGColorSpaceRef space, size_t height, enum ImageKind kind, IOSurfaceRef *keep, CGContextRef *keepContext)
{
    if (kind == kSurface) {
        *keep = createSurface(kWide, (int)height);
        *keepContext = CGIOSurfaceContextCreate(*keep, kWide, height, 8, 32, space, kBGRA);
        CGContextFlush(*keepContext);
        IOSurfaceLock(*keep, 0, NULL);
        fillRows(IOSurfaceGetBaseAddress(*keep), IOSurfaceGetBytesPerRow(*keep), height);
        IOSurfaceUnlock(*keep, 0, NULL);
        return CGIOSurfaceContextCreateImage(*keepContext);
    }
    if (kind == kGrayAlpha) {
        uint8_t *pixels = malloc(kWide * 2 * height);
        for (size_t y = 0; y < height; y++) {
            for (size_t x = 0; x < kWide; x++) {
                pixels[(y * kWide + x) * 2] = (uint8_t)(y & 0xff);
                pixels[(y * kWide + x) * 2 + 1] = 255;
            }
        }
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kWide * 2 * height, releaseRows);
        CGImageRef image = CGImageCreate(kWide, height, 8, 16, kWide * 2, gray, (CGBitmapInfo)kCGImageAlphaPremultipliedLast, provider, NULL,
            true, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(gray);
        return image;
    }
    uint8_t *pixels = malloc(kWide * 4 * height);
    fillRows(pixels, kWide * 4, height);
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kWide * 4 * height, releaseRows);
    CGImageRef image = CGImageCreate(kWide, height, 8, 32, kWide * 4, space, kBGRA, provider, NULL, kind != kNotInterpolating,
        kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    if (kind != kMasked)
        return image;
    uint8_t *maskBits = calloc(kWide, height);
    CGDataProviderRef maskProvider = CGDataProviderCreateWithData(NULL, maskBits, kWide * height, releaseRows);
    CGImageRef mask = CGImageMaskCreate(kWide, height, 8, 8, kWide, maskProvider, NULL, false);
    CGImageRef masked = CGImageCreateWithMask(image, mask);
    CGImageRelease(mask);
    CGDataProviderRelease(maskProvider);
    CGImageRelease(image);
    return masked;
}

// The BGRA pixel grey level g with alpha 255 lands on in a surface of `space`, for every g.
static uint8_t gGrayLevels[256][4];
static void learnGrayLevels(CGColorSpaceRef space)
{
    CGImageRef levels = createImage(space, 256, kGrayAlpha, NULL, NULL);
    IOSurfaceRef surface = createSurface(kWide, 256);
    CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, 256, 8, 32, space, kBGRA);
    CGContextClearRect(context, CGRectMake(0, 0, kWide, 256));
    CGContextDrawImage(context, CGRectMake(0, 0, kWide, 256), levels);
    CGContextFlush(context);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    for (int g = 0; g < 256; g++)
        memcpy(gGrayLevels[g], (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)g * IOSurfaceGetBytesPerRow(surface), 4);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    CGContextRelease(context);
    CFRelease(surface);
    CGImageRelease(levels);
}

typedef void (*DrawImageFn)(CGContextRef, CGRect, CGImageRef);
static DrawImageFn gDraw = CGContextDrawImage;

// 10.9's own CGContextDrawImage, reached by image lookup.
static DrawImageFn systemDrawImage(void)
{
    const struct mach_header *image = NSAddImage("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
        NSADDIMAGE_OPTION_RETURN_ON_ERROR);
    NSSymbol found = image ? NSLookupSymbolInImage(image, "_CGContextDrawImage", NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR) : 0;
    return found ? (DrawImageFn)NSAddressOfSymbol(found) : 0;
}

static double milliseconds(void)
{
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom)
        mach_timebase_info(&timebase);
    return mach_absolute_time() * (double)timebase.numer / timebase.denom / 1e6;
}

// Draws `image` one to one with its row `firstRow` along the top row of a surface `surfaceRows` tall, under a
// shadow `shadowRows` rows up or down and cast past the surface's side when `shadowed`, and answers how many of
// the surface's rows that the image covers hold another row than their own.
static size_t wrongRowsDrawing(CGImageRef image, enum ImageKind kind, CGColorSpaceRef space, int surfaceRows, int firstRow, bool shadowed,
    CGFloat shadowRows)
{
    int height = (int)CGImageGetHeight(image);
    IOSurfaceRef surface = createSurface(kWide, surfaceRows);
    CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, surfaceRows, 8, 32, space, kBGRA);
    CGContextClearRect(context, CGRectMake(0, 0, kWide, surfaceRows));
    if (shadowed) {
        CGColorRef black = CGColorCreateGenericGray(0, 1);
        CGContextSetShadowWithColor(context, CGSizeMake(100, shadowRows), 2, black);
        CGColorRelease(black);
    }
    gDraw(context, CGRectMake(0, surfaceRows + firstRow - height, kWide, height), image);
    CGContextFlush(context);
    size_t wrong = 0;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    for (int r = 0; r < surfaceRows; r++) {
        int row = firstRow + r;
        if (row < 0 || row >= height)
            continue;
        const uint8_t *p = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)r * IOSurfaceGetBytesPerRow(surface);
        bool exact = true;
        for (size_t x = 0; x < kWide; x++) {
            if (kind == kGrayAlpha)
                exact &= !memcmp(p + x * 4, gGrayLevels[row & 0xff], 4);
            else
                exact &= p[x * 4] == (row & 0xff) && p[x * 4 + 1] == (row >> 8) && p[x * 4 + 2] == 77 && p[x * 4 + 3] == 255;
        }
        wrong += !exact;
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    CGContextRelease(context);
    CFRelease(surface);
    return wrong;
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
    static const struct {
        int height, surfaceRows, firstRow;
    } wholeDraws[] = {
        { 8193, 8193, 0 }, { 10000, 10000, 0 }, { 12000, 16384, -1234 }, { 16000, 16000, 0 }, { 7795, 16384, -572 }, { 6113, 16384, -4319 },
    };
    learnGrayLevels(space);
    static const struct {
        enum ImageKind kind;
        bool shadowed;
        CGFloat shadowRows;
    } variants[] = {
        { kMemory, false, 0 }, { kSurface, false, 0 }, { kMasked, false, 0 }, { kNotInterpolating, false, 0 }, { kGrayAlpha, false, 0 },
        { kMemory, true, 0 }, { kSurface, true, 0 }, { kMemory, true, 3000 }, { kSurface, true, -9000 },
    };
    for (size_t v = 0; v < sizeof variants / sizeof variants[0]; v++) {
        double started = milliseconds();
        for (size_t i = 0; i < sizeof wholeDraws / sizeof wholeDraws[0]; i++) {
            IOSurfaceRef keep = NULL;
            CGContextRef keepContext = NULL;
            CGImageRef image = createImage(space, wholeDraws[i].height, variants[v].kind, &keep, &keepContext);
            size_t wrong = wrongRowsDrawing(image, variants[v].kind, space, wholeDraws[i].surfaceRows, wholeDraws[i].firstRow, variants[v].shadowed,
                variants[v].shadowRows);
            char what[160];
            snprintf(what, sizeof what, "%s%s image %d rows tall at row %d of %d: %zu rows out of place", kKindNames[variants[v].kind],
                variants[v].shadowed ? (variants[v].shadowRows ? ", shadowed far," : ", shadowed,") : "", wholeDraws[i].height, -wholeDraws[i].firstRow,
                wholeDraws[i].surfaceRows, wrong);
            check(!wrong, what);
            CGImageRelease(image);
            if (keepContext)
                CGContextRelease(keepContext);
            if (keep)
                CFRelease(keep);
        }
        // A shadow thousands of rows away is drawn with the image's own draws, not tiles that grow with its reach.
        if (variants[v].shadowRows) {
            double elapsed = milliseconds() - started;
            char what[160];
            snprintf(what, sizeof what, "%s images under a shadow %g rows away: six draws in %.0f ms, under 5 s", kKindNames[variants[v].kind],
                variants[v].shadowRows, elapsed);
            check(elapsed < 5000, what);
        }
    }

    // 10.9's own draw steps only the rows the clip leaves: windows of 512 and 1024 rows of a 16000-row image, cut by
    // the surface, land in place with no help, as 1024 * 16000 < 2^24.
    {
        DrawImageFn system = systemDrawImage();
        check(system != NULL, "10.9's CGContextDrawImage resolves");
        IOSurfaceRef keep = NULL;
        CGContextRef keepContext = NULL;
        CGImageRef image = createImage(space, 16000, kMemory, &keep, &keepContext);
        static const int windows[][2] = { { 512, 2000 }, { 512, 9000 }, { 512, 15000 }, { 1024, 1000 }, { 1024, 14000 } };
        for (size_t w = 0; system && w < sizeof windows / sizeof windows[0]; w++) {
            gDraw = system;
            size_t wrong = wrongRowsDrawing(image, kMemory, space, windows[w][0], windows[w][1], false, 0);
            gDraw = CGContextDrawImage;
            char what[160];
            snprintf(what, sizeof what, "10.9's draw of 16000 rows into %d rows from row %d: %zu rows out of place", windows[w][0], windows[w][1], wrong);
            check(!wrong, what);
        }
        CGImageRelease(image);
    }

    for (int fromSurface = 0; fromSurface < 2; fromSurface++) {
        IOSurfaceRef keep = NULL;
        CGContextRef keepContext = NULL;
        CGImageRef image = createImage(space, 9021, fromSurface ? kSurface : kMemory, &keep, &keepContext);
        size_t tiles = 0, wrongTiles = 0;
        for (int top = 0; top < 9021; top += 512, tiles++)
            wrongTiles += wrongRowsDrawing(image, fromSurface ? kSurface : kMemory, space, 512, top, false, 0) != 0;
        char what[160];
        snprintf(what, sizeof what, "%s image 9021 rows tall into 512-row tiles: %zu of %zu tiles out of place", fromSurface ? "surface" : "memory",
            wrongTiles, tiles);
        check(!wrongTiles, what);
        CGImageRelease(image);
        if (keepContext)
            CGContextRelease(keepContext);
        if (keep)
            CFRelease(keep);
    }
    // Oversized images shrunk to fit small surfaces paint every pixel.
    {
        static const int shrinks[][4] = { { 8, 40000, 8, 200 }, { 40000, 8, 400, 8 }, { 8, 20000, 8, 1000 }, { 8, 20000, 8, 16000 } };
        for (size_t i = 0; i < sizeof shrinks / sizeof shrinks[0]; i++) {
            int imageWidth = shrinks[i][0], imageHeight = shrinks[i][1], surfaceWidth = shrinks[i][2], surfaceHeight = shrinks[i][3];
            uint8_t *pixels = malloc((size_t)imageWidth * imageHeight * 4);
            for (size_t p = 0; p < (size_t)imageWidth * imageHeight; p++) {
                pixels[p * 4] = 0;
                pixels[p * 4 + 1] = 200;
                pixels[p * 4 + 2] = 0;
                pixels[p * 4 + 3] = 255;
            }
            CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)imageWidth * imageHeight * 4, releaseRows);
            CGImageRef image = CGImageCreate(imageWidth, imageHeight, 8, 32, imageWidth * 4, space, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            IOSurfaceRef surface = createSurface(surfaceWidth, surfaceHeight);
            CGContextRef context = CGIOSurfaceContextCreate(surface, surfaceWidth, surfaceHeight, 8, 32, space, kBGRA);
            CGContextClearRect(context, CGRectMake(0, 0, surfaceWidth, surfaceHeight));
            CGContextDrawImage(context, CGRectMake(0, 0, surfaceWidth, surfaceHeight), image);
            CGContextFlush(context);
            if (i == 0) {
                // The same draw in two passes, each clipped to part of the surface, into a surface of another colour
                // space, which matches the image's rows each pass reads.
                CGColorSpaceRef otherSpace = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
                CGContextRelease(context);
                context = CGIOSurfaceContextCreate(surface, surfaceWidth, surfaceHeight, 8, 32, otherSpace, kBGRA);
                CGContextClearRect(context, CGRectMake(0, 0, surfaceWidth, surfaceHeight));
                for (int pass = 0; pass < 2; pass++) {
                    CGContextSaveGState(context);
                    CGContextClipToRect(context, CGRectMake(0, pass ? surfaceHeight * 2 / 3 : 0, surfaceWidth, pass ? surfaceHeight - surfaceHeight * 2 / 3 : surfaceHeight * 2 / 3));
                    CGContextDrawImage(context, CGRectMake(0, 0, surfaceWidth, surfaceHeight), image);
                    CGContextRestoreGState(context);
                    CGContextFlush(context);
                }
                CGColorSpaceRelease(otherSpace);
            }
            size_t unpainted = 0;
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            for (int y = 0; y < surfaceHeight; y++) {
                const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
                for (int x = 0; x < surfaceWidth; x++)
                    unpainted += row[x * 4 + 3] < 250;
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            char what[160];
            snprintf(what, sizeof what, "%dx%d image shrunk into %dx%d%s: %zu of %d pixels unpainted", imageWidth, imageHeight, surfaceWidth, surfaceHeight,
                i == 0 ? " in two passes of another space" : "", unpainted, surfaceWidth * surfaceHeight);
            check(!unpainted, what);
            CGContextRelease(context);
            CFRelease(surface);
            CGImageRelease(image);
        }
    }

    // Shrunk images that paint at alpha one half -- a translucent image, an opaque one under a context alpha of one half,
    // and a translucent one turned a quarter -- leave every pixel at alpha 128, painted once.
    {
        static const int heights[] = { 20000, 40000, 100000 };
        static const int shrunkHeights[] = { 333, 2500, 7777, 12345, 16000 };
        static const char *const ways[] = { "translucent", "opaque under alpha 0.5", "translucent, turned" };
        for (int way = 0; way < 3; way++) {
            for (size_t h = 0; h < sizeof heights / sizeof heights[0]; h++) {
                int imageHeight = heights[h];
                uint8_t alpha = way == 1 ? 255 : 128;
                uint8_t *pixels = malloc((size_t)kWide * imageHeight * 4);
                for (size_t q = 0; q < (size_t)kWide * imageHeight; q++) {
                    pixels[q * 4] = 0;
                    pixels[q * 4 + 1] = (uint8_t)(alpha * 100 / 255);
                    pixels[q * 4 + 2] = 0;
                    pixels[q * 4 + 3] = alpha;
                }
                CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)kWide * imageHeight * 4, releaseRows);
                CGImageRef image = CGImageCreate(kWide, imageHeight, 8, 32, kWide * 4, space, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
                CGDataProviderRelease(provider);
                size_t wrongScales = 0, scales = 0, worst = 0;
                for (size_t d = 0; d < sizeof shrunkHeights / sizeof shrunkHeights[0]; d++) {
                    int shrunk = shrunkHeights[d];
                    int surfaceWidth = way == 2 ? shrunk : kWide, surfaceHeight = way == 2 ? kWide : shrunk;
                    IOSurfaceRef surface = createSurface(surfaceWidth, surfaceHeight);
                    CGContextRef context = CGIOSurfaceContextCreate(surface, surfaceWidth, surfaceHeight, 8, 32, space, kBGRA);
                    CGContextClearRect(context, CGRectMake(0, 0, surfaceWidth, surfaceHeight));
                    if (way == 1)
                        CGContextSetAlpha(context, 0.5);
                    if (way == 2) {
                        CGContextTranslateCTM(context, surfaceWidth, 0);
                        CGContextRotateCTM(context, M_PI / 2);
                    }
                    CGContextDrawImage(context, CGRectMake(0, 0, kWide, shrunk), image);
                    CGContextFlush(context);
                    size_t off = 0;
                    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
                    for (int y = 0; y < surfaceHeight; y++) {
                        const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
                        for (int x = 0; x < surfaceWidth; x++)
                            off += abs((int)row[x * 4 + 3] - 128) > 1;
                    }
                    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
                    scales++;
                    wrongScales += off != 0;
                    worst = off > worst ? off : worst;
                    CGContextRelease(context);
                    CFRelease(surface);
                }
                char what[160];
                snprintf(what, sizeof what, "%s image %d rows tall shrunk: %zu of %zu heights off alpha 128 (most %zu pixels)", ways[way], imageHeight,
                    wrongScales, scales, worst);
                check(!wrongScales, what);
                CGImageRelease(image);
            }
        }
    }

    // Turned and extremely shrunk draws against CoreGraphics' bitmap draw of the same image under the same transform.
    {
        static const struct {
            int imageWidth, imageHeight, rectWidth, rectHeight, surfaceSide;
            double degrees;
            int alpha;
            double shadowX, shadowY;
            // Placed: the surface is placedWidth x surfaceSide, and the rect's origin, before the turn, at placeX, placeY.
            int placedWidth;
            double placeX, placeY;
        } turns[] = {
            { 64, 16000, 64, 2000, 1500, 30, 128, 0, 0, 0, 0, 0 }, { 64, 16000, 64, 2000, 1500, 45, 128, 0, 0, 0, 0, 0 }, { 8, 40000, 8, 60, 64, 45, 128, 0, 0, 0, 0, 0 },
            { 40000, 8, 8, 8, 16, 0, 255, 0, 0, 0, 0, 0 }, { 40000, 8, 3, 8, 16, 0, 255, 0, 0, 0, 0, 0 }, { 20000, 2000, 400, 40, 600, 10, 255, 0, 0, 0, 0, 0 }, { 20000, 2000, 400, 40, 600, 10, 128, 0, 0, 0, 0, 0 },
            { 17000, 17000, 400, 400, 500, 0, 255, 0, 0, 0, 0, 0 }, { 8, 20000, 8, 1000, 1100, 0, 255, 0, 0, 0, 0, 0 }, { 20000, 2000, 400, 40, 700, 0, 255, 0, -150, 0, 0, 0 },
            { 20000, 2000, 400, 40, 700, 0, 255, 150, 0, 0, 0, 0 },
            // An image's top edge enlarged twice with its columns 2000 to 3000 in view, and one to one at a half-pixel
            // origin with its columns 2000 to 4000 in view; its right edge with its rows 2000 to 4000 in view at a
            // half-pixel origin; and its top-right corner 10 pixels in from the surface's, turned 5 degrees, with
            // columns 18000 on in view.
            { 20000, 20, 40000, 40, 200, 0, 255, 0, 0, 2000, -4000, 150 }, { 20000, 9, 20000, 9, 200, 0, 255, 0, 0, 2000, -2000, 181.5 },
            { 9, 20001, 9, 20001, 2000, 0, 255, 0, 0, 200, 181.5, -16001 }, { 20000, 20, 20000, 40, 2000, 5, 255, 0, 0, 2000, -1, -1 },
        };
        for (size_t t = 0; t < sizeof turns / sizeof turns[0]; t++) {
            int iw = turns[t].imageWidth, ih = turns[t].imageHeight, side = turns[t].surfaceSide;
            uint8_t alpha = (uint8_t)turns[t].alpha;
            uint8_t *pixels = malloc((size_t)iw * ih * 4);
            for (size_t q = 0; q < (size_t)iw * ih; q++) {
                pixels[q * 4] = 0;
                pixels[q * 4 + 1] = (uint8_t)(alpha * 150 / 255);
                pixels[q * 4 + 2] = 0;
                pixels[q * 4 + 3] = alpha;
            }
            CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, (size_t)iw * ih * 4, releaseRows);
            CGImageRef image = CGImageCreate(iw, ih, 8, 32, iw * 4, space, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            int sideWide = turns[t].placedWidth ? turns[t].placedWidth : side;
            IOSurfaceRef surface = createSurface(sideWide, side);
            CGContextRef context = CGIOSurfaceContextCreate(surface, sideWide, side, 8, 32, space, kBGRA);
            CGContextRef reference = CGBitmapContextCreate(NULL, sideWide, side, 8, sideWide * 4, space, kBGRA);
            CGContextClearRect(context, CGRectMake(0, 0, sideWide, side));
            CGContextRef both[2] = { context, reference };
            for (int k = 0; k < 2; k++) {
                if (turns[t].shadowX || turns[t].shadowY) {
                    CGColorRef black = CGColorCreateGenericGray(0, 1);
                    CGContextSetShadowWithColor(both[k], CGSizeMake(turns[t].shadowX, turns[t].shadowY), 2, black);
                    CGColorRelease(black);
                }
                if (turns[t].placedWidth) {
                    double placeX = turns[t].placeX, placeY = turns[t].placeY;
                    if (placeX == -1 && placeY == -1) {
                        // The turned rect's far corner 10 pixels in from the surface's.
                        double turn = turns[t].degrees * M_PI / 180;
                        placeX = sideWide - 10 - (cos(turn) * turns[t].rectWidth - sin(turn) * turns[t].rectHeight);
                        placeY = side - 10 - (sin(turn) * turns[t].rectWidth + cos(turn) * turns[t].rectHeight);
                    }
                    CGContextTranslateCTM(both[k], placeX, placeY);
                    CGContextRotateCTM(both[k], turns[t].degrees * M_PI / 180);
                    CGContextDrawImage(both[k], CGRectMake(0, 0, turns[t].rectWidth, turns[t].rectHeight), image);
                    continue;
                }
                CGContextTranslateCTM(both[k], side / 2.0, side / 2.0);
                CGContextRotateCTM(both[k], turns[t].degrees * M_PI / 180);
                CGContextTranslateCTM(both[k], -turns[t].rectWidth / 2.0, -turns[t].rectHeight / 2.0);
                CGContextDrawImage(both[k], CGRectMake(0, 0, turns[t].rectWidth, turns[t].rectHeight), image);
            }
            CGContextFlush(context);
            size_t off = 0;
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            for (int y = 0; y < side; y++) {
                const uint8_t *row = (const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface);
                const uint8_t *referenceRow = (const uint8_t *)CGBitmapContextGetData(reference) + (size_t)y * CGBitmapContextGetBytesPerRow(reference);
                for (int x = 0; x < sideWide; x++)
                    off += abs((int)row[x * 4 + 3] - (int)referenceRow[x * 4 + 3]) > 40;
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            char what[160];
            snprintf(what, sizeof what, "%dx%d image at alpha %d into %dx%d turned %g degrees%s%s: %zu pixels off the bitmap's", iw, ih, turns[t].alpha,
                turns[t].rectWidth, turns[t].rectHeight, turns[t].degrees, turns[t].shadowX || turns[t].shadowY ? ", shadow 150 away" : "",
                turns[t].placedWidth ? ", edge in view" : "", off);
            check(!off, what);
            CGContextRelease(reference);
            CGContextRelease(context);
            CFRelease(surface);
            CGImageRelease(image);
        }
    }

    // Tiles of another colour space: the rows are matched, so only their coverage is compared.
    {
        IOSurfaceRef keep = NULL;
        CGContextRef keepContext = NULL;
        CGImageRef image = createImage(space, 9021, kSurface, &keep, &keepContext);
        CGColorSpaceRef tileSpace = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
        size_t tiles = 0, unpaintedTiles = 0;
        for (int top = 0; top < 9021; top += 512, tiles++) {
            IOSurfaceRef surface = createSurface(kWide, 512);
            CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, 512, 8, 32, tileSpace, kBGRA);
            CGContextClearRect(context, CGRectMake(0, 0, kWide, 512));
            CGContextTranslateCTM(context, 0, 512);
            CGContextScaleCTM(context, 2, -2);
            CGContextTranslateCTM(context, 0, -top / 2.0);
            CGContextClipToRect(context, CGRectMake(0, top / 2.0, kWide / 2.0, 256));
            CGContextTranslateCTM(context, 0, 9021 / 2.0);
            CGContextScaleCTM(context, 1, -1);
            CGContextDrawImage(context, CGRectMake(0, 0, kWide / 2.0, 9021 / 2.0), image);
            CGContextFlush(context);
            size_t unpainted = 0;
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            for (int r = 0; r < 512 && top + r < 9021; r++)
                unpainted += ((const uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)r * IOSurfaceGetBytesPerRow(surface))[3] != 255;
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            unpaintedTiles += unpainted != 0;
            CGContextRelease(context);
            CFRelease(surface);
        }
        char what[160];
        snprintf(what, sizeof what, "surface image 9021 rows tall as flipped 2x tiles of another space: %zu of %zu tiles unpainted",
            unpaintedTiles, tiles);
        check(!unpaintedTiles, what);
        CGColorSpaceRelease(tileSpace);
        CGImageRelease(image);
        CGContextRelease(keepContext);
        CFRelease(keep);
    }
    CGColorSpaceRelease(space);
    return failures ? 1 : 0;
}
#endif
