/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "image_preprocess.h"

#include <algorithm>
#include <cmath>

namespace OHOS {
namespace ScreenParser {

namespace {

inline uint8_t SampleRgb(const uint8_t *rgb, int32_t srcW, int32_t srcH, int32_t x, int32_t y,
                         int32_t c) {
    x = std::max(0, std::min(x, srcW - 1));
    y = std::max(0, std::min(y, srcH - 1));
    return rgb[(static_cast<size_t>(y) * srcW + x) * 3 + c];
}

}  // namespace

std::vector<uint8_t> BilinearResizeRgb(const uint8_t *rgb, int32_t srcW, int32_t srcH, int32_t dstW,
                                       int32_t dstH) {
    std::vector<uint8_t> out;
    if (rgb == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return out;
    }
    out.resize(static_cast<size_t>(dstW) * dstH * 3);

    const float xRatio = static_cast<float>(srcW) / dstW;
    const float yRatio = static_cast<float>(srcH) / dstH;

    for (int32_t dy = 0; dy < dstH; ++dy) {
        float sy = (dy + 0.5f) * yRatio - 0.5f;
        int32_t y0 = static_cast<int32_t>(std::floor(sy));
        float wy = sy - y0;
        int32_t y1 = y0 + 1;
        for (int32_t dx = 0; dx < dstW; ++dx) {
            float sx = (dx + 0.5f) * xRatio - 0.5f;
            int32_t x0 = static_cast<int32_t>(std::floor(sx));
            float wx = sx - x0;
            int32_t x1 = x0 + 1;
            for (int32_t c = 0; c < 3; ++c) {
                float top0 = SampleRgb(rgb, srcW, srcH, x0, y0, c);
                float top1 = SampleRgb(rgb, srcW, srcH, x1, y0, c);
                float bot0 = SampleRgb(rgb, srcW, srcH, x0, y1, c);
                float bot1 = SampleRgb(rgb, srcW, srcH, x1, y1, c);
                float top = top0 + (top1 - top0) * wx;
                float bot = bot0 + (bot1 - bot0) * wx;
                float value = top + (bot - top) * wy;
                int32_t iv = static_cast<int32_t>(std::lround(value));
                out[(static_cast<size_t>(dy) * dstW + dx) * 3 + c] =
                    static_cast<uint8_t>(std::max(0, std::min(255, iv)));
            }
        }
    }
    return out;
}

ImageTensor ResizeAndNormalize(const uint8_t *rgb, int32_t srcW, int32_t srcH, int32_t dstW,
                               int32_t dstH, const NormalizeParams &params) {
    ImageTensor tensor;
    std::vector<uint8_t> resized = BilinearResizeRgb(rgb, srcW, srcH, dstW, dstH);
    if (resized.empty()) {
        return tensor;
    }
    tensor.channels = 3;
    tensor.height = dstH;
    tensor.width = dstW;
    const size_t planeSize = static_cast<size_t>(dstW) * dstH;
    tensor.data.resize(planeSize * 3);

    for (int32_t c = 0; c < 3; ++c) {
        float mean = params.mean[c];
        float std = params.std[c] == 0.0f ? 1.0f : params.std[c];
        for (size_t i = 0; i < planeSize; ++i) {
            float pixel = resized[i * 3 + c] / 255.0f;
            tensor.data[static_cast<size_t>(c) * planeSize + i] = (pixel - mean) / std;
        }
    }
    return tensor;
}

}  // namespace ScreenParser
}  // namespace OHOS
