#include "ImageDecodeUtils.hpp"

#include <algorithm>
#include <cstddef>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace ImageDecodeUtils {

#if defined(__APPLE__)

namespace {

void assign_error(std::string* error, const std::string& message)
{
    if (error) {
        *error = message;
    }
}

} // namespace

bool decode_image_to_png(const std::string& image_path_utf8,
                         int max_width,
                         int max_height,
                         DecodedImage& decoded,
                         std::string* error)
{
    decoded = DecodedImage{};
    if (image_path_utf8.empty()) {
        assign_error(error, "Image path is empty.");
        return false;
    }

    CFStringRef path_string = CFStringCreateWithCString(kCFAllocatorDefault,
                                                        image_path_utf8.c_str(),
                                                        kCFStringEncodingUTF8);
    if (!path_string) {
        assign_error(error, "Image path is not valid UTF-8.");
        return false;
    }

    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                                 path_string,
                                                 kCFURLPOSIXPathStyle,
                                                 false);
    CFRelease(path_string);
    if (!url) {
        assign_error(error, "Could not build an image file URL.");
        return false;
    }

    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!source) {
        assign_error(error, "ImageIO could not open the image file.");
        return false;
    }

    CGImageRef image = nullptr;
    const int longest_side = std::max(max_width, max_height);
    if (longest_side > 0) {
        const void* keys[] = {
            kCGImageSourceCreateThumbnailFromImageAlways,
            kCGImageSourceThumbnailMaxPixelSize,
            kCGImageSourceCreateThumbnailWithTransform
        };
        int size = longest_side;
        CFNumberRef pixel_size = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &size);
        const void* values[] = {kCFBooleanTrue, pixel_size, kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault,
                                                     keys,
                                                     values,
                                                     3,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        image = CGImageSourceCreateThumbnailAtIndex(source, 0, options);
        CFRelease(options);
        CFRelease(pixel_size);
    }

    if (!image) {
        image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    }
    CFRelease(source);

    if (!image) {
        assign_error(error, "ImageIO could not decode the image.");
        return false;
    }

    decoded.width = static_cast<int>(CGImageGetWidth(image));
    decoded.height = static_cast<int>(CGImageGetHeight(image));

    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    if (!data) {
        CGImageRelease(image);
        assign_error(error, "Could not allocate PNG output buffer.");
        return false;
    }

    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr);
    if (!destination) {
        CFRelease(data);
        CGImageRelease(image);
        assign_error(error, "Could not create a PNG encoder.");
        return false;
    }

    CGImageDestinationAddImage(destination, image, nullptr);
    const bool finished = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    CGImageRelease(image);

    if (!finished) {
        CFRelease(data);
        assign_error(error, "Could not encode the image as PNG.");
        return false;
    }

    const CFIndex length = CFDataGetLength(data);
    const UInt8* bytes = CFDataGetBytePtr(data);
    if (length <= 0 || !bytes) {
        CFRelease(data);
        assign_error(error, "PNG encoder produced no output.");
        return false;
    }

    decoded.png_bytes.assign(bytes, bytes + static_cast<std::size_t>(length));
    CFRelease(data);
    return true;
}

#else

bool decode_image_to_png(const std::string&,
                         int,
                         int,
                         DecodedImage& decoded,
                         std::string* error)
{
    decoded = DecodedImage{};
    assign_error(error,
                 "Image decoding for visual analysis is unavailable in this headless build.");
    return false;
}

#endif

} // namespace ImageDecodeUtils
