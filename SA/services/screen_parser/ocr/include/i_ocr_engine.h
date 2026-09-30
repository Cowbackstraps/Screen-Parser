/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// On-device OCR (text recognition) engine abstraction.
//
// The production backend is CoreVisionOcrEngine, which drives the OpenHarmony
// core_vision TextRecognition system ability (ohos.ai.ocr) entirely on-device.
// Tests inject a fake engine that returns canned text blocks, so the OCR fusion
// pipeline can be verified offline.

#ifndef FOUNDATION_SCREENPARSER_OCR_I_OCR_ENGINE_H
#define FOUNDATION_SCREENPARSER_OCR_I_OCR_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "screen_schema.h"

namespace OHOS {
namespace ScreenParser {

// A single recognized text region. Bounds are normalized to the 0-1000 space
// (consistent with ScreenNode) so they can be fused with VLM nodes directly.
struct OcrTextBlock {
    std::string text;
    Bounds bounds;
    float confidence = 0.0f;

    json::Value ToJson() const;
};

class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;

    // Acquire the OCR backend. Returns false (error set) on failure.
    virtual bool Init(std::string &error) = 0;
    virtual bool IsReady() const = 0;

    // Recognize text in an RGBA buffer (width * height * 4 bytes). Recognized
    // pixel boxes are normalized to 0-1000 using width/height. Returns false
    // (error set) on failure; an empty `out` with true simply means no text.
    virtual bool Recognize(const uint8_t *rgba, int32_t width, int32_t height,
                           std::vector<OcrTextBlock> &out, std::string &error) = 0;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_OCR_I_OCR_ENGINE_H
