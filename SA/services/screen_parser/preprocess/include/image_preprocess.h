/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Image preprocessing for the vision encoder input.
// Converts a captured RGB buffer into a normalized float tensor (CHW layout)
// at the resolution expected by the model. Pure CPU code with no OHOS
// dependency so it can be unit-tested off-device.

#ifndef FOUNDATION_SCREENPARSER_PREPROCESS_IMAGE_PREPROCESS_H
#define FOUNDATION_SCREENPARSER_PREPROCESS_IMAGE_PREPROCESS_H

#include <cstdint>
#include <vector>

namespace OHOS {
namespace ScreenParser {

// Normalized image tensor in CHW layout (channels x height x width).
struct ImageTensor {
    std::vector<float> data;
    int32_t channels = 0;
    int32_t height = 0;
    int32_t width = 0;

    bool Empty() const { return data.empty(); }
};

struct NormalizeParams {
    float mean[3] = {0.5f, 0.5f, 0.5f};
    float std[3] = {0.5f, 0.5f, 0.5f};
};

// Bilinear-resize an RGB buffer (srcW x srcH, 3 bytes/pixel) to dstW x dstH and
// normalize into a CHW float tensor: (pixel/255 - mean) / std.
ImageTensor ResizeAndNormalize(const uint8_t *rgb, int32_t srcW, int32_t srcH, int32_t dstW,
                               int32_t dstH, const NormalizeParams &params);

// Bilinear resize only (RGB -> RGB), used by tests and the base64 preview path.
std::vector<uint8_t> BilinearResizeRgb(const uint8_t *rgb, int32_t srcW, int32_t srcH, int32_t dstW,
                                       int32_t dstH);

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_PREPROCESS_IMAGE_PREPROCESS_H
