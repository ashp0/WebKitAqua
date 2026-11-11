// The CGContextDrawImage replacement (polyfills/c/CoreGraphics.c) for an image whose colour space is not
// the IOSurface context's. A linear-sRGB image of every 8-bit level is drawn into an sRGB IOSurface at
// sizes both sides of the point where 10.9's QuartzCore stops matching exactly, and every level must land
// within one level of its exact sRGB encoding. The same draws through 10.9's own definition show the
// colour cube at the larger sizes, which is the premise. Image alpha (opaque, premultiplied) and 16-bit
// components each take the route. A translucent image's levels must land within one level of their exact
// match too, in an IOSurface and in an 8-bit bitmap, which 10.9's own 8-bit bitmap route misses by more; a
// straight-alpha one as that route matches it, within two levels. Translucent images of other matrix-shaper pairs land within one level of
// CoreGraphics' own match of them widened to 16 bits, and so do one in a space ColorSync describes by tables,
// drawn into an 8-bit bitmap.
// An image wider than the renderer's texture limit is matched across the parts it is drawn in, and one whose
// draws read only some of its rows, however tall, is matched draw by draw as they are. A partially
// decoded CMYK JPEG, built as JPEGImageDecoder::createNativeImage builds it (an opaque CMYK
// image under CGImageCreateWithMask), paints only its decoded rows.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CGContextRef CGIOSurfaceContextCreate(IOSurfaceRef, size_t, size_t, size_t, size_t, CGColorSpaceRef, CGBitmapInfo);
CGImageRef CGIOSurfaceContextCreateImage(CGContextRef);
typedef void (*DrawImageFn)(CGContextRef, CGRect, CGImageRef);

static const CGBitmapInfo kBGRA = (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;

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

static CGColorSpaceRef createLinearSRGB(void)
{
    const CGFloat white[3] = { 0.9505, 1.0, 1.0890 }, black[3] = { 0, 0, 0 }, gamma[3] = { 1, 1, 1 };
    const CGFloat primaries[9] = { 0.4124564, 0.2126729, 0.0193339, 0.3575761, 0.7151522, 0.1191920, 0.1804375, 0.0721750, 0.9503041 };
    return CGColorSpaceCreateCalibratedRGB(white, black, gamma, primaries);
}

// Level x % 256 in every colour component of column x, in the requested layout.
static CGImageRef createLevelsImage(CGColorSpaceRef space, size_t width, size_t height, bool sixteenBit, bool opaque)
{
    size_t bitsPerComponent = sixteenBit ? 16 : 8, pixelBytes = sixteenBit ? 8 : 4, rowBytes = width * pixelBytes;
    CGBitmapInfo info = sixteenBit
        ? kCGBitmapByteOrder16Host | (CGBitmapInfo)(opaque ? kCGImageAlphaNoneSkipLast : kCGImageAlphaPremultipliedLast)
        : kCGBitmapByteOrder32Little | (CGBitmapInfo)(opaque ? kCGImageAlphaNoneSkipFirst : kCGImageAlphaPremultipliedFirst);
    unsigned char *pixels = calloc(height, rowBytes);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            unsigned level = x % 256;
            if (sixteenBit) {
                uint16_t *p = (uint16_t *)(pixels + y * rowBytes + x * 8);
                p[0] = p[1] = p[2] = (uint16_t)(level * 257);
                p[3] = 0xffff;
            } else {
                unsigned char *p = pixels + y * rowBytes + x * 4;
                p[0] = p[1] = p[2] = (unsigned char)level;
                p[3] = 0xff;
            }
        }
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, height * rowBytes, NULL);
    CGImageRef image = CGImageCreate(width, height, bitsPerComponent, pixelBytes * 8, rowBytes, space, info, provider, NULL, false,
        kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    return image;
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

// The green byte of each of the first 256 columns of row 0.
static void drawIntoSurface(DrawImageFn draw, CGColorSpaceRef space, CGImageRef image, unsigned char out[256])
{
    size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    IOSurfaceRef surface = createSurface(width, height);
    CGContextRef context = CGIOSurfaceContextCreate(surface, width, height, 8, 32, space, kBGRA);
    draw(context, CGRectMake(0, 0, width, height), image);
    CGContextFlush(context);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    const unsigned char *base = IOSurfaceGetBaseAddress(surface);
    size_t rowBytes = IOSurfaceGetBytesPerRow(surface);
    for (size_t x = 0; x < 256 && x < width; x++)
        out[x] = base[(height - 1) * rowBytes + x * 4 + 1];
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    CGContextRelease(context);
    CFRelease(surface);
}

// The sRGB encoding of a linear value, as an 8-bit level.
static int exactEncoding(double linearValue)
{
    double encoded = linearValue <= 0.0031308 ? linearValue * 12.92 : 1.055 * pow(linearValue, 1 / 2.4) - 0.055;
    return (int)lround(encoded * 255);
}

static void archiveDraw(CGContextRef context, CGRect rect, CGImageRef image)
{
    CGContextDrawImage(context, rect, image);
}

int main(int argc, char **argv)
{
    DrawImageFn system = systemDrawImage();
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
    }
    CGColorSpaceRef linear = createLinearSRGB();
    CGColorSpaceRef sRGB = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!system || !linear || !sRGB)
        return 1;

    static const struct { size_t width, height; bool sixteenBit, opaque; const char *name; } cases[] = {
        { 64, 64, false, false, "64x64 premultiplied 8-bit" },
        { 256, 256, false, false, "256x256 premultiplied 8-bit" },
        { 256, 256, false, true, "256x256 opaque 8-bit" },
        { 256, 256, true, false, "256x256 premultiplied 16-bit" },
        { 1024, 1, false, false, "1024x1 premultiplied 8-bit" },
    };
    bool systemDiverges = false;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CGImageRef image = createLevelsImage(linear, cases[i].width, cases[i].height, cases[i].sixteenBit, cases[i].opaque);
        unsigned char actual[256] = { 0 }, stock[256] = { 0 };
        drawIntoSurface(archiveDraw, sRGB, image, actual);
        drawIntoSurface(system, sRGB, image, stock);
        size_t columns = cases[i].width < 256 ? cases[i].width : 256;
        int worst = 0;
        for (size_t level = 0; level < columns; level++) {
            int exact = exactEncoding(level / 255.0);
            int error = abs((int)actual[level] - exact);
            if (error > worst)
                worst = error;
            if (abs((int)stock[level] - exact) > 1)
                systemDiverges = true;
        }
        char what[128];
        snprintf(what, sizeof(what), "%s: every level within one of its exact encoding", cases[i].name);
        check(worst <= 1, what);
        if (worst > 1)
            printf("    level 1: actual %u exact %d\n", actual[1], exactEncoding(1 / 255.0));
        CGImageRelease(image);
    }
    check(systemDiverges, "10.9's own definition colour-matches some of these through its cube");

    // Every premultiplied green level g <= a at every alpha a, one per pixel of a 256x256 image.
    {
        enum { kSide = 256 };
        unsigned char *pixels = calloc(kSide * kSide, 4);
        for (size_t a = 1; a < kSide; a++) {
            for (size_t g = 0; g < kSide; g++) {
                unsigned char *p = pixels + (kSide - 1 - a) * kSide * 4 + g * 4;
                p[1] = (unsigned char)(g <= a ? g : a);
                p[3] = (unsigned char)a;
            }
        }
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kSide * kSide * 4, NULL);
        CGImageRef image = CGImageCreate(kSide, kSide, 8, 32, kSide * 4, linear, kBGRA, provider, NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        // Route 0 is an IOSurface, route 1 an 8-bit bitmap, both through the archive; route 2 the bitmap
        // through 10.9's own definition.
        int worst[3] = { 0, 0, 0 };
        for (int route = 0; route < 3; route++) {
            unsigned char *out = calloc(kSide * kSide, 4);
            if (route) {
                CGContextRef context = CGBitmapContextCreate(out, kSide, kSide, 8, kSide * 4, sRGB, kBGRA);
                CGContextSetBlendMode(context, kCGBlendModeCopy);
                (route == 1 ? archiveDraw : system)(context, CGRectMake(0, 0, kSide, kSide), image);
                CGContextRelease(context);
            } else {
                IOSurfaceRef surface = createSurface(kSide, kSide);
                CGContextRef context = CGIOSurfaceContextCreate(surface, kSide, kSide, 8, 32, sRGB, kBGRA);
                CGContextSetBlendMode(context, kCGBlendModeCopy);
                CGContextDrawImage(context, CGRectMake(0, 0, kSide, kSide), image);
                CGContextFlush(context);
                IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
                for (size_t y = 0; y < kSide; y++)
                    memcpy(out + y * kSide * 4, (const unsigned char *)IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface), kSide * 4);
                IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
                CGContextRelease(context);
                CFRelease(surface);
            }
            for (size_t a = 1; a < kSide; a++) {
                for (size_t g = 0; g <= a; g++) {
                    double linearValue = (double)g / (double)a;
                    double encoded = linearValue <= 0.0031308 ? linearValue * 12.92 : 1.055 * pow(linearValue, 1 / 2.4) - 0.055;
                    int exact = (int)lround(encoded * (double)a);
                    int error = abs((int)out[(kSide - 1 - a) * kSide * 4 + g * 4 + 1] - exact);
                    if (error > worst[route])
                        worst[route] = error;
                }
            }
            free(out);
        }
        printf("    worst level error: IOSurface %d, bitmap %d, 10.9's bitmap route %d\n", worst[0], worst[1], worst[2]);
        check(worst[0] <= 1, "a translucent linear-sRGB image matches within one level of exact");
        check(worst[1] <= 1, "and does into an 8-bit bitmap");
        check(worst[2] > 1, "10.9's own 8-bit bitmap route misses some of it by more");
        CGImageRelease(image);
        free(pixels);
    }

    // Pairs of other matrix-shaper spaces: translucent pixels of every alpha drawn into an IOSurface land
    // within one level of CoreGraphics' own match of the image widened to 16 bits, Display P3 colours
    // outside sRGB's gamut included.
    {
        enum { kSide = 256 };
        CFDataRef adobeProfile = NULL;
        FILE *file = fopen("/System/Library/ColorSync/Profiles/AdobeRGB1998.icc", "rb");
        if (file) {
            unsigned char bytes[8192];
            size_t length = fread(bytes, 1, sizeof(bytes), file);
            fclose(file);
            adobeProfile = CFDataCreate(NULL, bytes, (CFIndex)length);
        }
        CGColorSpaceRef adobe = adobeProfile ? CGColorSpaceCreateWithICCProfile(adobeProfile) : NULL;
        CGColorSpaceRef generic = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
        CGColorSpaceRef displayP3 = CGColorSpaceCreateWithName(CFSTR("kCGColorSpaceDisplayP3"));
        struct { CGColorSpaceRef from, to; const char *what; } pairs[] = {
            { adobe, generic, "a translucent Adobe RGB image matches Generic RGB as a 16-bit match does" },
            { displayP3, sRGB, "a translucent Display P3 image matches sRGB as a 16-bit match does" },
            { sRGB, linear, "a translucent sRGB image matches linear sRGB as a 16-bit match does" },
        };
        unsigned char *pixels = calloc(kSide * kSide, 4);
        unsigned seed = 1;
        for (size_t y = 0; y < kSide; y++) {
            for (size_t x = 0; x < kSide; x++) {
                unsigned char *p = pixels + (y * kSide + x) * 4;
                unsigned alpha = (unsigned)x;
                for (int c = 0; c < 3; c++) {
                    seed = seed * 1103515245 + 12345;
                    p[c] = (unsigned char)(alpha ? (seed >> 16) % (alpha + 1) : 0);
                }
                p[3] = (unsigned char)alpha;
            }
        }
        for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
            if (!pairs[i].from || !pairs[i].to) {
                check(0, pairs[i].what);
                continue;
            }
            CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kSide * kSide * 4, NULL);
            CGImageRef image = CGImageCreate(kSide, kSide, 8, 32, kSide * 4, pairs[i].from, kBGRA, provider, NULL, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);

            const CGBitmapInfo wide = kCGBitmapByteOrder16Host | (CGBitmapInfo)kCGImageAlphaPremultipliedLast;
            CGContextRef widened = CGBitmapContextCreate(NULL, kSide, kSide, 16, 0, pairs[i].from, wide);
            CGContextSetBlendMode(widened, kCGBlendModeCopy);
            CGContextDrawImage(widened, CGRectMake(0, 0, kSide, kSide), image);
            CGImageRef widenedImage = CGBitmapContextCreateImage(widened);
            CGContextRef reference = CGBitmapContextCreate(NULL, kSide, kSide, 16, 0, pairs[i].to, wide);
            CGContextSetBlendMode(reference, kCGBlendModeCopy);
            CGContextDrawImage(reference, CGRectMake(0, 0, kSide, kSide), widenedImage);

            IOSurfaceRef surface = createSurface(kSide, kSide);
            CGContextRef context = CGIOSurfaceContextCreate(surface, kSide, kSide, 8, 32, pairs[i].to, kBGRA);
            CGContextSetBlendMode(context, kCGBlendModeCopy);
            CGContextDrawImage(context, CGRectMake(0, 0, kSide, kSide), image);
            CGContextFlush(context);
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            const unsigned char *base = IOSurfaceGetBaseAddress(surface);
            size_t surfaceRowBytes = IOSurfaceGetBytesPerRow(surface);
            const uint16_t *expected = CGBitmapContextGetData(reference);
            size_t expectedRow = CGBitmapContextGetBytesPerRow(reference) / 2;
            int worst = 0;
            for (size_t y = 0; y < kSide; y++) {
                for (size_t x = 0; x < kSide; x++) {
                    for (int c = 0; c < 4; c++) {
                        static const int bgra[4] = { 2, 1, 0, 3 };
                        int want = (int)lround(expected[y * expectedRow + x * 4 + c] / 257.0);
                        int error = abs((int)base[y * surfaceRowBytes + x * 4 + bgra[c]] - want);
                        if (error > worst)
                            worst = error;
                    }
                }
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            printf("    worst level difference: %d\n", worst);
            check(worst <= 1, pairs[i].what);
            CGContextRelease(context);
            CFRelease(surface);
            CGContextRelease(reference);
            CGImageRelease(widenedImage);
            CGContextRelease(widened);
            CGImageRelease(image);
        }
        free(pixels);
        if (adobe)
            CGColorSpaceRelease(adobe);
        if (adobeProfile)
            CFRelease(adobeProfile);
        CGColorSpaceRelease(generic);
        if (displayP3)
            CGColorSpaceRelease(displayP3);
    }

    // A space ColorSync describes by tables (an sRGB profile with a Lab connection space and LUT tags, the
    // first argument): a translucent image in it drawn into an 8-bit Generic RGB bitmap lands within one level
    // of CoreGraphics' own match of it widened to 16 bits, which 10.9's own bitmap route misses by more.
    if (argc > 1) {
        enum { kSide = 256 };
        CFDataRef tableProfile = NULL;
        FILE *file = fopen(argv[1], "rb");
        if (file) {
            static unsigned char bytes[1 << 17];
            size_t length = fread(bytes, 1, sizeof(bytes), file);
            fclose(file);
            tableProfile = CFDataCreate(NULL, bytes, (CFIndex)length);
        }
        CGColorSpaceRef table = tableProfile ? CGColorSpaceCreateWithICCProfile(tableProfile) : NULL;
        CGColorSpaceRef generic = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
        if (!table) {
            check(0, "the table-described profile loads");
        } else {
            unsigned char *pixels = calloc(kSide * kSide, 4);
            unsigned seed = 7;
            for (size_t y = 0; y < kSide; y++) {
                for (size_t x = 0; x < kSide; x++) {
                    unsigned char *p = pixels + (y * kSide + x) * 4;
                    for (int c = 0; c < 3; c++) {
                        seed = seed * 1103515245 + 12345;
                        p[c] = (unsigned char)(x ? (seed >> 16) % (x + 1) : 0);
                    }
                    p[3] = (unsigned char)x;
                }
            }
            CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kSide * kSide * 4, NULL);
            CGImageRef image = CGImageCreate(kSide, kSide, 8, 32, kSide * 4, table, kBGRA, provider, NULL, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            const CGBitmapInfo wide = kCGBitmapByteOrder16Host | (CGBitmapInfo)kCGImageAlphaPremultipliedLast;
            CGContextRef widened = CGBitmapContextCreate(NULL, kSide, kSide, 16, 0, table, wide);
            CGContextSetBlendMode(widened, kCGBlendModeCopy);
            CGContextDrawImage(widened, CGRectMake(0, 0, kSide, kSide), image);
            CGImageRef widenedImage = CGBitmapContextCreateImage(widened);
            CGContextRef reference = CGBitmapContextCreate(NULL, kSide, kSide, 16, 0, generic, wide);
            CGContextSetBlendMode(reference, kCGBlendModeCopy);
            CGContextDrawImage(reference, CGRectMake(0, 0, kSide, kSide), widenedImage);
            const uint16_t *expected = CGBitmapContextGetData(reference);
            size_t expectedRow = CGBitmapContextGetBytesPerRow(reference) / 2;
            int worst[2] = { 0, 0 };
            for (int route = 0; route < 2; route++) {
                unsigned char *out = calloc(kSide * kSide, 4);
                CGContextRef context = CGBitmapContextCreate(out, kSide, kSide, 8, kSide * 4, generic, kBGRA);
                CGContextSetBlendMode(context, kCGBlendModeCopy);
                (route ? system : archiveDraw)(context, CGRectMake(0, 0, kSide, kSide), image);
                CGContextRelease(context);
                for (size_t y = 0; y < kSide; y++) {
                    for (size_t x = 0; x < kSide; x++) {
                        for (int c = 0; c < 4; c++) {
                            static const int bgra[4] = { 2, 1, 0, 3 };
                            int want = (int)lround(expected[y * expectedRow + x * 4 + c] / 257.0);
                            int error = abs((int)out[(y * kSide + x) * 4 + bgra[c]] - want);
                            if (error > worst[route])
                                worst[route] = error;
                        }
                    }
                }
                free(out);
            }
            printf("    table-described space into an 8-bit bitmap: worst level difference %d, 10.9's own route %d\n", worst[0], worst[1]);
            check(worst[0] <= 1, "a translucent image in a table-described space matches as a 16-bit match does");
            check(worst[1] > 1, "10.9's own 8-bit bitmap route misses some of it by more");
            CGContextRelease(reference);
            CGImageRelease(widenedImage);
            CGContextRelease(widened);
            CGImageRelease(image);
            free(pixels);
            CGColorSpaceRelease(table);
        }
        CGColorSpaceRelease(generic);
        if (tableProfile)
            CFRelease(tableProfile);
    }

    // An image wider than QuartzCore's texture limit is drawn from parts of its per-space image: the visible
    // columns 13000 to 16999 of a 17000x2 linear-sRGB ramp, drawn twice into a 4000-wide surface, land within
    // one level of their exact encoding both times.
    {
        enum { kWide = 17000, kTall = 2, kSurface = 4000, kFirst = 13000 };
        CGImageRef image = createLevelsImage(linear, kWide, kTall, false, true);
        IOSurfaceRef surface = createSurface(kSurface, kTall);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kSurface, kTall, 8, 32, sRGB, kBGRA);
        int worst = 0;
        for (int draw = 0; draw < 2; draw++) {
            CGContextClearRect(context, CGRectMake(0, 0, kSurface, kTall));
            CGContextDrawImage(context, CGRectMake(-kFirst, 0, kWide, kTall), image);
            CGContextFlush(context);
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
            const unsigned char *base = IOSurfaceGetBaseAddress(surface);
            for (size_t x = 0; x < kSurface; x++) {
                int error = abs((int)base[x * 4 + 1] - exactEncoding(((x + kFirst) % 256) / 255.0));
                if (error > worst)
                    worst = error;
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        }
        printf("    oversized image worst level error: %d\n", worst);
        check(worst <= 1, "an image over the texture limit is matched in the parts drawn of it");
        CGContextRelease(context);
        CFRelease(surface);
        CGImageRelease(image);
    }

    // An image whose draws each read only some of its rows has those rows matched draw by draw. Painted in
    // three bands as tiles are -- a translucent linear-sRGB image of varied pixels, 8- and 16-bit, at scales
    // that magnify, shrink and keep it, at every interpolation quality, into an sRGB IOSurface and an sRGB
    // 8-bit bitmap -- it lands what the same bands land once every row of it is matched, within one level or
    // what 10.9 itself varies by. The reference is another newly made image.
    {
        enum { kImageWidth = 300, kImageHeight = 900, kSurfaceWidth = 640, kSurfaceHeight = 420 };
        static const double scales[] = { 1, 2, 1.37, 0.78, 0.31 };
        static const CGInterpolationQuality qualities[] = { kCGInterpolationDefault, kCGInterpolationNone, kCGInterpolationMedium, kCGInterpolationHigh };
        static const CGRect bands[] = { { { 37, 23 }, { 261, 47 } }, { { 37, 70 }, { 261, 113 } }, { { 37, 183 }, { 261, 200 } } };
        size_t rowBytes8 = kImageWidth * 4, rowBytes16 = kImageWidth * 8;
        unsigned char *pixels8 = malloc(kImageHeight * rowBytes8);
        uint16_t *pixels16 = malloc(kImageHeight * rowBytes16);
        for (size_t y = 0; y < kImageHeight; y++) {
            for (size_t x = 0; x < kImageWidth; x++) {
                unsigned alpha = 40 + (x * 7 + y * 13) % 216;
                unsigned colour[3] = { (x * 31 + y * 3) % 256, (x * 5 + y * 17) % 256, (x * y) % 256 };
                unsigned char *p = pixels8 + y * rowBytes8 + x * 4;
                uint16_t *q = pixels16 + (y * kImageWidth + x) * 4;
                for (int c = 0; c < 3; c++) {
                    p[2 - c] = (unsigned char)(colour[c] * alpha / 255);
                    q[c] = (uint16_t)(colour[c] * alpha / 255 * 257);
                }
                p[3] = (unsigned char)alpha;
                q[3] = (uint16_t)(alpha * 257);
            }
        }
        // The reference has every row matched first, by a draw that reads one row and then one that reads all
        // of them. A 16-bit image takes the whole matched image on both sides, so its differences are 10.9's own.
        int worst[2] = { 0, 0 }, unmatchedByRows = 0;
        {
            for (int sixteen = 0; sixteen < 2; sixteen++) {
                for (size_t s = 0; s < sizeof scales / sizeof *scales; s++) {
                    for (size_t q = 0; q < sizeof qualities / sizeof *qualities; q++) {
                        for (int bitmapDestination = 0; bitmapDestination < 2; bitmapDestination++) {
                            unsigned char *results[2];
                            for (int drawByDraw = 0; drawByDraw < 2; drawByDraw++) {
                                CGDataProviderRef provider = sixteen
                                    ? CGDataProviderCreateWithData(NULL, pixels16, kImageHeight * rowBytes16, NULL)
                                    : CGDataProviderCreateWithData(NULL, pixels8, kImageHeight * rowBytes8, NULL);
                                CGImageRef image = sixteen
                                    ? CGImageCreate(kImageWidth, kImageHeight, 16, 64, rowBytes16, linear,
                                        kCGBitmapByteOrder16Host | (CGBitmapInfo)kCGImageAlphaPremultipliedLast, provider, NULL, true, kCGRenderingIntentDefault)
                                    : CGImageCreate(kImageWidth, kImageHeight, 8, 32, rowBytes8, linear, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
                                CGDataProviderRelease(provider);
                                // One row of each image is drawn first, and then all of the reference.
                                IOSurfaceRef scratch = createSurface(kImageWidth, kImageHeight);
                                CGContextRef whole = CGIOSurfaceContextCreate(scratch, kImageWidth, kImageHeight, 8, 32, sRGB, kBGRA);
                                CGContextSaveGState(whole);
                                CGContextClipToRect(whole, CGRectMake(0, 0, kImageWidth, 1));
                                CGContextDrawImage(whole, CGRectMake(0, 0, kImageWidth, kImageHeight), image);
                                CGContextRestoreGState(whole);
                                if (!drawByDraw)
                                    CGContextDrawImage(whole, CGRectMake(0, 0, kImageWidth, kImageHeight), image);
                                CGContextFlush(whole);
                                CGContextRelease(whole);
                                CFRelease(scratch);
                                IOSurfaceRef surface = bitmapDestination ? NULL : createSurface(kSurfaceWidth, kSurfaceHeight);
                                unsigned char *bitmapPixels = bitmapDestination ? calloc(kSurfaceHeight, kSurfaceWidth * 4) : NULL;
                                CGContextRef context = bitmapDestination
                                    ? CGBitmapContextCreate(bitmapPixels, kSurfaceWidth, kSurfaceHeight, 8, kSurfaceWidth * 4, sRGB, kBGRA)
                                    : CGIOSurfaceContextCreate(surface, kSurfaceWidth, kSurfaceHeight, 8, 32, sRGB, kBGRA);
                                CGContextClearRect(context, CGRectMake(0, 0, kSurfaceWidth, kSurfaceHeight));
                                CGContextSetInterpolationQuality(context, qualities[q]);
                                // Edges on whole pixels: 10.9 paints a partly covered edge pixel of a shrunk image by
                                // what it has drawn of the image before, whichever way it is matched.
                                CGRect rect = CGRectMake(5, -4, kImageWidth * scales[s], kImageHeight * scales[s]);
                                for (size_t b = 0; b < sizeof bands / sizeof *bands; b++) {
                                    CGContextSaveGState(context);
                                    CGContextClipToRect(context, bands[b]);
                                    CGContextDrawImage(context, rect, image);
                                    CGContextRestoreGState(context);
                                }
                                CGContextFlush(context);
                                if (!sixteen && !bitmapDestination && !objc_getAssociatedObject((id)image, sel_registerName("wk_ioSurfaceImageRows")))
                                    unmatchedByRows++;
                                results[drawByDraw] = malloc(kSurfaceHeight * kSurfaceWidth * 4);
                                if (bitmapDestination)
                                    memcpy(results[drawByDraw], bitmapPixels, kSurfaceHeight * kSurfaceWidth * 4);
                                else {
                                    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
                                    const unsigned char *base = IOSurfaceGetBaseAddress(surface);
                                    size_t surfaceRowBytes = IOSurfaceGetBytesPerRow(surface);
                                    for (size_t y = 0; y < kSurfaceHeight; y++)
                                        memcpy(results[drawByDraw] + y * kSurfaceWidth * 4, base + y * surfaceRowBytes, kSurfaceWidth * 4);
                                    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
                                }
                                CGContextRelease(context);
                                if (surface)
                                    CFRelease(surface);
                                free(bitmapPixels);
                                CGImageRelease(image);
                            }
                            for (size_t i = 0; i < kSurfaceHeight * kSurfaceWidth * 4; i++) {
                                int difference = abs((int)results[0][i] - (int)results[1][i]);
                                if (difference > worst[sixteen])
                                    worst[sixteen] = difference;
                            }
                            free(results[0]);
                            free(results[1]);
                        }
                    }
                }
            }
        }
        free(pixels8);
        free(pixels16);
        printf("    banded draws, worst level difference from every row matched first: %d (16-bit, 10.9's own: %d)\n", worst[0], worst[1]);
        check(!unmatchedByRows, "an 8-bit image drawn into an IOSurface has its rows matched");
        check(worst[1] <= 3, "10.9 itself varies by no more than three levels between these draws");
        check(worst[0] <= (worst[1] > 1 ? worst[1] : 1), "rows of a matched image matched draw by draw land what all of them matched do");
    }

    // An image of an sRGB IOSurface context, its pixels written by the CPU, drawn into an Adobe RGB surface and
    // then into an sRGB one, lands its own colour in the second; and a draw of it is kept when its own context
    // is filled over right after.
    {
        CGColorSpaceRef adobe = CGColorSpaceCreateWithName(kCGColorSpaceAdobeRGB1998);
        IOSurfaceRef source = createSurface(512, 512);
        IOSurfaceLock(source, 0, NULL);
        for (size_t y = 0; y < 512; y++) {
            for (size_t x = 0; x < 512; x++) {
                unsigned char *q = (unsigned char *)IOSurfaceGetBaseAddress(source) + y * IOSurfaceGetBytesPerRow(source) + x * 4;
                q[0] = 35;
                q[1] = 51;
                q[2] = 234;
                q[3] = 255;
            }
        }
        IOSurfaceUnlock(source, 0, NULL);
        CGContextRef sourceContext = CGIOSurfaceContextCreate(source, 512, 512, 8, 32, sRGB, kBGRA);
        CGImageRef snapshot = CGIOSurfaceContextCreateImage(sourceContext);
        IOSurfaceRef other = createSurface(4, 4), same = createSurface(4, 4);
        CGContextRef otherContext = CGIOSurfaceContextCreate(other, 4, 4, 8, 32, adobe, kBGRA);
        CGContextRef sameContext = CGIOSurfaceContextCreate(same, 4, 4, 8, 32, sRGB, kBGRA);
        CGContextDrawImage(otherContext, CGRectMake(0, 0, 4, 4), snapshot);
        CGContextFlush(otherContext);
        CGContextDrawImage(sameContext, CGRectMake(0, 0, 4, 4), snapshot);
        CGContextSetRGBFillColor(sourceContext, 0, 0, 1, 1);
        CGContextFillRect(sourceContext, CGRectMake(0, 0, 512, 512));
        CGContextFlush(sourceContext);
        CGContextFlush(sameContext);
        IOSurfaceLock(same, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *p = IOSurfaceGetBaseAddress(same);
        printf("    sRGB surface after the Adobe RGB draw: BGRA %d,%d,%d,%d\n", p[0], p[1], p[2], p[3]);
        check(abs(p[2] - 234) <= 1 && abs(p[1] - 51) <= 1 && abs(p[0] - 35) <= 1,
            "an image of an IOSurface context lands its own colour in its own space after another space");
        IOSurfaceUnlock(same, kIOSurfaceLockReadOnly, NULL);
        CGImageRelease(snapshot);
        CGContextRelease(sameContext);
        CGContextRelease(otherContext);
        CGContextRelease(sourceContext);
        CFRelease(same);
        CFRelease(other);
        CFRelease(source);
        CGColorSpaceRelease(adobe);
    }

    // An image of a linear-sRGB IOSurface context holding a ramp 3000 rows tall, drawn at its own size into an
    // sRGB surface showing rows 2000 to 2099 only: every row lands within one level of its exact encoding.
    {
        enum { kWide = 64, kTall = 3000, kFirst = 2000, kRows = 100 };
        IOSurfaceRef source = createSurface(kWide, kTall);
        IOSurfaceLock(source, 0, NULL);
        unsigned char *pixels = IOSurfaceGetBaseAddress(source);
        for (size_t y = 0; y < kTall; y++) {
            for (size_t x = 0; x < kWide; x++) {
                unsigned char *p = pixels + y * IOSurfaceGetBytesPerRow(source) + x * 4;
                p[0] = p[1] = p[2] = (unsigned char)(y % 256);
                p[3] = 0xff;
            }
        }
        IOSurfaceUnlock(source, 0, NULL);
        CGContextRef sourceContext = CGIOSurfaceContextCreate(source, kWide, kTall, 8, 32, linear, kBGRA);
        CGImageRef image = CGIOSurfaceContextCreateImage(sourceContext);
        IOSurfaceRef surface = createSurface(kWide, kRows);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, kRows, 8, 32, sRGB, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, kWide, kRows));
        CGContextDrawImage(context, CGRectMake(0, kRows + kFirst - kTall, kWide, kTall), image);
        CGContextFlush(context);
        int worst = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        for (size_t r = 0; r < kRows; r++) {
            for (size_t x = 0; x < kWide; x++) {
                int error = abs((int)base[r * IOSurfaceGetBytesPerRow(surface) + x * 4 + 1] - exactEncoding(((kFirst + r) % 256) / 255.0));
                if (error > worst)
                    worst = error;
            }
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        printf("    image of an IOSurface context, rows drawn: worst level error %d\n", worst);
        check(worst <= 1, "an image of an IOSurface context drawn in part matches the rows drawn");
        check(objc_getAssociatedObject((id)image, sel_registerName("wk_ioSurfaceImageRows")) != NULL, "and has its rows matched");
        CGImageRelease(image);
        CGContextRelease(context);
        CGContextRelease(sourceContext);
        CFRelease(surface);
        CFRelease(source);
    }

    // An image of an sRGB IOSurface context holding a vertical gradient, drawn into two bands of an Adobe RGB
    // surface with its context filled over and flushed between them: both bands land 10.9's own bitmap-route
    // match of the gradient, within one level.
    {
        enum { kWide = 32, kTall = 2048, kShown = 256 };
        CGColorSpaceRef adobe = CGColorSpaceCreateWithName(kCGColorSpaceAdobeRGB1998);
        IOSurfaceRef source = createSurface(kWide, kTall);
        unsigned char *gradient = malloc(kWide * kTall * 4);
        for (size_t y = 0; y < kTall; y++) {
            for (size_t x = 0; x < kWide; x++) {
                unsigned char *p = gradient + (y * kWide + x) * 4;
                p[0] = (unsigned char)(255 - y % 256);
                p[1] = (unsigned char)(y * 7 % 256);
                p[2] = (unsigned char)(y % 256);
                p[3] = 0xff;
            }
        }
        IOSurfaceLock(source, 0, NULL);
        for (size_t y = 0; y < kTall; y++)
            memcpy((unsigned char *)IOSurfaceGetBaseAddress(source) + y * IOSurfaceGetBytesPerRow(source), gradient + y * kWide * 4, kWide * 4);
        IOSurfaceUnlock(source, 0, NULL);
        CGContextRef sourceContext = CGIOSurfaceContextCreate(source, kWide, kTall, 8, 32, sRGB, kBGRA);
        CGImageRef snapshot = CGIOSurfaceContextCreateImage(sourceContext);

        // Image rows 1000 to 1255 lie along the destination; its bottom band shows 1128 to 1255, its top 1000 to 1127.
        CGRect rect = CGRectMake(0, kShown + 1000 - kTall, kWide, kTall);
        IOSurfaceRef surface = createSurface(kWide, kShown);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, kShown, 8, 32, adobe, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, kWide, kShown));
        CFArrayRef record = (CFArrayRef)(void *)objc_getAssociatedObject((id)snapshot, sel_registerName("wk_imageSurface"));
        uint32_t recordedSeed = 0;
        if (record)
            CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(record, 1), kCFNumberSInt32Type, &recordedSeed);
        check(record && IOSurfaceGetSeed(source) == recordedSeed, "an image of an IOSurface context keeps its surface's seed, unchanged before a redraw");
        CGContextSaveGState(context);
        CGContextClipToRect(context, CGRectMake(0, 0, kWide, kShown / 2));
        CGContextDrawImage(context, rect, snapshot);
        CGContextRestoreGState(context);
        CGContextFlush(context);
        CGContextSetRGBFillColor(sourceContext, 0, 0, 1, 1);
        CGContextFillRect(sourceContext, CGRectMake(0, 0, kWide, kTall));
        CGContextFlush(sourceContext);
        check(IOSurfaceGetSeed(source) != recordedSeed, "and changed after one");
        CGContextSaveGState(context);
        CGContextClipToRect(context, CGRectMake(0, kShown / 2, kWide, kShown / 2));
        CGContextDrawImage(context, rect, snapshot);
        CGContextRestoreGState(context);
        CGContextFlush(context);

        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, gradient, kWide * kTall * 4, NULL);
        CGImageRef memoryImage = CGImageCreate(kWide, kTall, 8, 32, kWide * 4, sRGB, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        unsigned char *expected = calloc(kShown, kWide * 4);
        CGContextRef bitmap = CGBitmapContextCreate(expected, kWide, kShown, 8, kWide * 4, adobe, kBGRA);
        system(bitmap, rect, memoryImage);
        int worst = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        for (size_t r = 0; r < kShown; r++) {
            for (size_t i = 0; i < kWide * 4; i++) {
                int difference = abs((int)base[r * IOSurfaceGetBytesPerRow(surface) + i] - (int)expected[r * kWide * 4 + i]);
                if (difference > worst)
                    worst = difference;
            }
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        printf("    image of an IOSurface context in two bands around a refill: worst level difference %d\n", worst);
        check(worst <= 1, "an image of an IOSurface context keeps its pixels for draws after its context is drawn into");
        CGContextRelease(bitmap);
        free(expected);
        CGImageRelease(memoryImage);
        CGImageRelease(snapshot);
        CGContextRelease(context);
        CGContextRelease(sourceContext);
        CFRelease(surface);
        CFRelease(source);
        free(gradient);
        CGColorSpaceRelease(adobe);
    }

    // An sRGB image of an IOSurface context 20000 rows tall, its pixels written by the CPU, drawn into an Adobe
    // RGB surface and then into an sRGB one, lands its own colour in the second.
    {
        enum { kWide = 8, kTall = 20000 };
        CGColorSpaceRef adobe = CGColorSpaceCreateWithName(kCGColorSpaceAdobeRGB1998);
        IOSurfaceRef source = createSurface(kWide, kTall);
        IOSurfaceLock(source, 0, NULL);
        for (size_t y = 0; y < kTall; y++) {
            for (size_t x = 0; x < kWide; x++) {
                unsigned char *q = (unsigned char *)IOSurfaceGetBaseAddress(source) + y * IOSurfaceGetBytesPerRow(source) + x * 4;
                q[0] = 35;
                q[1] = 51;
                q[2] = 234;
                q[3] = 255;
            }
        }
        IOSurfaceUnlock(source, 0, NULL);
        CGContextRef sourceContext = CGIOSurfaceContextCreate(source, kWide, kTall, 8, 32, sRGB, kBGRA);
        CGImageRef snapshot = CGIOSurfaceContextCreateImage(sourceContext);
        IOSurfaceRef other = createSurface(kWide, 4), same = createSurface(kWide, 4);
        CGContextRef otherContext = CGIOSurfaceContextCreate(other, kWide, 4, 8, 32, adobe, kBGRA);
        CGContextRef sameContext = CGIOSurfaceContextCreate(same, kWide, 4, 8, 32, sRGB, kBGRA);
        CGContextDrawImage(otherContext, CGRectMake(0, 4 - kTall, kWide, kTall), snapshot);
        CGContextFlush(otherContext);
        CGContextDrawImage(sameContext, CGRectMake(0, 4 - kTall, kWide, kTall), snapshot);
        CGContextFlush(sameContext);
        IOSurfaceLock(same, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *p = IOSurfaceGetBaseAddress(same);
        printf("    20000-row image, sRGB surface after the Adobe RGB draw: BGRA %d,%d,%d,%d\n", p[0], p[1], p[2], p[3]);
        check(abs(p[2] - 234) <= 1 && abs(p[1] - 51) <= 1 && abs(p[0] - 35) <= 1, "and does so 20000 rows tall");
        IOSurfaceUnlock(same, kIOSurfaceLockReadOnly, NULL);
        CGImageRelease(snapshot);
        CGContextRelease(sameContext);
        CGContextRelease(otherContext);
        CGContextRelease(sourceContext);
        CFRelease(same);
        CFRelease(other);
        CFRelease(source);
        CGColorSpaceRelease(adobe);
    }

    // A linear-sRGB ramp 20000 rows tall, over the software renderer's texture limit, drawn at its own size with
    // only rows 15000 to 15099 inside the surface: every row lands within one level of its exact encoding.
    {
        enum { kWide = 4, kTall = 20000, kFirst = 15000, kRows = 100 };
        unsigned char *pixels = malloc(kWide * kTall * 4);
        for (size_t y = 0; y < kTall; y++) {
            for (size_t x = 0; x < kWide; x++) {
                unsigned char *p = pixels + (y * kWide + x) * 4;
                p[0] = p[1] = p[2] = (unsigned char)(y % 256);
                p[3] = 0xff;
            }
        }
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kWide * kTall * 4, NULL);
        CGImageRef image = CGImageCreate(kWide, kTall, 8, 32, kWide * 4, linear, kBGRA, provider, NULL, true, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        IOSurfaceRef surface = createSurface(kWide, kRows);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kWide, kRows, 8, 32, sRGB, kBGRA);
        CGContextClearRect(context, CGRectMake(0, 0, kWide, kRows));
        // Image row kFirst + r lies along surface row r counted from the top.
        CGContextDrawImage(context, CGRectMake(0, kRows + kFirst - kTall, kWide, kTall), image);
        CGContextFlush(context);
        int worst = 0;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        for (size_t r = 0; r < kRows; r++) {
            int error = abs((int)base[r * IOSurfaceGetBytesPerRow(surface) + 1] - exactEncoding(((kFirst + r) % 256) / 255.0));
            if (error > worst)
                worst = error;
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        printf("    tall image, rows drawn: worst level error %d\n", worst);
        check(worst <= 1, "an image taller than the texture limit drawn in part matches the rows drawn");
        CGContextRelease(context);
        CFRelease(surface);
        CGImageRelease(image);
        free(pixels);
    }

    // Straight alpha: every colour level c at every alpha a, matched within one level of exact.
    {
        enum { kSide = 256 };
        unsigned char *pixels = calloc(kSide * kSide, 4);
        for (size_t a = 1; a < kSide; a++) {
            for (size_t c = 0; c < kSide; c++) {
                unsigned char *p = pixels + (kSide - 1 - a) * kSide * 4 + c * 4;
                p[0] = p[1] = p[2] = (unsigned char)c;
                p[3] = (unsigned char)a;
            }
        }
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kSide * kSide * 4, NULL);
        CGImageRef image = CGImageCreate(kSide, kSide, 8, 32, kSide * 4, linear, (CGBitmapInfo)kCGImageAlphaLast, provider, NULL, false,
            kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        IOSurfaceRef surface = createSurface(kSide, kSide);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kSide, kSide, 8, 32, sRGB, kBGRA);
        CGContextSetBlendMode(context, kCGBlendModeCopy);
        CGContextDrawImage(context, CGRectMake(0, 0, kSide, kSide), image);
        CGContextFlush(context);
        CGContextRef bitmap = CGBitmapContextCreate(NULL, kSide, kSide, 8, kSide * 4, sRGB, kBGRA);
        CGContextSetBlendMode(bitmap, kCGBlendModeCopy);
        CGContextDrawImage(bitmap, CGRectMake(0, 0, kSide, kSide), image);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        size_t rowBytes = IOSurfaceGetBytesPerRow(surface);
        bool sameAsBitmap = true;
        for (size_t y = 0; y < kSide; y++)
            sameAsBitmap &= !memcmp(base + y * rowBytes, (const unsigned char *)CGBitmapContextGetData(bitmap) + y * kSide * 4, kSide * 4);
        CGContextRelease(bitmap);
        int worst = 0;
        for (size_t a = 1; a < kSide; a++) {
            for (size_t c = 0; c < kSide; c++) {
                double linearValue = c / 255.0;
                double encoded = linearValue <= 0.0031308 ? linearValue * 12.92 : 1.055 * pow(linearValue, 1 / 2.4) - 0.055;
                int exact = (int)lround(encoded * (double)a);
                int error = abs((int)base[(kSide - 1 - a) * rowBytes + c * 4 + 1] - exact);
                if (error > worst)
                    worst = error;
            }
        }
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        printf("    worst level error, straight alpha: %d\n", worst);
        check(sameAsBitmap, "a straight-alpha linear-sRGB image matches as CoreGraphics' bitmap route does");
        check(worst <= 2, "which is within two levels of exact");
        CGContextRelease(context);
        CFRelease(surface);
        CGImageRelease(image);
        free(pixels);
    }

    // A partially decoded CMYK JPEG: an opaque CMYK image whose first `decoded` rows are covered by a
    // 1-pixel-wide mask (0 paints, 255 leaves the destination), drawn over opaque white.
    {
        enum { kSide = 128, kDecoded = 40 };
        CGColorSpaceRef cmyk = CGColorSpaceCreateWithName(kCGColorSpaceGenericCMYK);
        unsigned char *pixels = calloc(kSide * kSide, 4);
        for (size_t i = 0; i < kSide * kSide; i++)
            pixels[i * 4 + 0] = 255;   // full cyan
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, kSide * kSide * 4, NULL);
        CGImageRef image = CGImageCreate(kSide, kSide, 8, 32, kSide * 4, cmyk, (CGBitmapInfo)kCGImageAlphaNone, provider, NULL, false,
            kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        unsigned char coverage[kSide];
        memset(coverage, 255, sizeof(coverage));
        memset(coverage, 0, kDecoded);
        CFDataRef coverageData = CFDataCreate(NULL, coverage, sizeof(coverage));
        CGDataProviderRef maskProvider = CGDataProviderCreateWithCFData(coverageData);
        CGImageRef mask = CGImageMaskCreate(1, kSide, 8, 8, 1, maskProvider, NULL, false);
        CGImageRef masked = CGImageCreateWithMask(image, mask);
        IOSurfaceRef surface = createSurface(kSide, kSide);
        CGContextRef context = CGIOSurfaceContextCreate(surface, kSide, kSide, 8, 32, sRGB, kBGRA);
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);
        CGContextFillRect(context, CGRectMake(0, 0, kSide, kSide));
        CGContextDrawImage(context, CGRectMake(0, 0, kSide, kSide), masked);
        CGContextFlush(context);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        size_t rowBytes = IOSurfaceGetBytesPerRow(surface);
        const unsigned char *decodedPixel = base + 10 * rowBytes + 64 * 4;
        const unsigned char *pendingPixel = base + 100 * rowBytes + 64 * 4;
        printf("    masked CMYK JPEG: decoded row BGRA %u,%u,%u,%u; pending row BGRA %u,%u,%u,%u\n", decodedPixel[0], decodedPixel[1],
            decodedPixel[2], decodedPixel[3], pendingPixel[0], pendingPixel[1], pendingPixel[2], pendingPixel[3]);
        check(decodedPixel[2] < 128 && decodedPixel[0] > 128, "a masked CMYK image paints its decoded rows");
        check(pendingPixel[0] == 255 && pendingPixel[1] == 255 && pendingPixel[2] == 255, "and leaves the rows its mask excludes");
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        CGContextRelease(context);
        CFRelease(surface);
        CGImageRelease(masked);
        CGImageRelease(mask);
        CGDataProviderRelease(maskProvider);
        CFRelease(coverageData);
        CGImageRelease(image);
        free(pixels);
        CGColorSpaceRelease(cmyk);
    }

    CGColorSpaceRelease(linear);
    CGColorSpaceRelease(sRGB);
    return failures ? 1 : 0;
}
