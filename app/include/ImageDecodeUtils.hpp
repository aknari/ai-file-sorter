/**
 * @file ImageDecodeUtils.hpp
 * @brief Shared image decoding helpers for visual analysis.
 *
 * The visual-analysis path only needs PNG bytes for the multimodal runtime, so
 * this helper stays free of QtGui: Qt GUI image classes would otherwise force
 * the headless engine to link QtWidgets/QtGui.
 */
#pragma once

#include <string>
#include <vector>

namespace ImageDecodeUtils {

/**
 * @brief Decoded image payload ready for the multimodal runtime.
 */
struct DecodedImage {
    /** @brief PNG-encoded image bytes. */
    std::vector<unsigned char> png_bytes;
    /** @brief Decoded pixel width, when known. */
    int width{0};
    /** @brief Decoded pixel height, when known. */
    int height{0};

    /**
     * @brief Returns whether decoding produced usable bytes.
     * @return True when PNG bytes are present.
     */
    bool valid() const { return !png_bytes.empty(); }
};

/**
 * @brief Decode an image file and encode it as PNG bytes.
 * @param image_path_utf8 Path to the image file as UTF-8 text.
 * @param max_width Optional maximum width; values <= 0 keep the original size.
 * @param max_height Optional maximum height; values <= 0 keep the original size.
 * @param decoded Receives the decoded payload.
 * @param error Receives a human-readable failure reason when decoding fails.
 * @return True when the image was decoded and encoded.
 */
bool decode_image_to_png(const std::string& image_path_utf8,
                         int max_width,
                         int max_height,
                         DecodedImage& decoded,
                         std::string* error = nullptr);

} // namespace ImageDecodeUtils
