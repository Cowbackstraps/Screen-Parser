/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// core_vision-backed OCR engine (on-device text recognition).
//
// When SCREENPARSER_ENABLE_OCR is defined this wraps the OpenHarmony
// core_vision TextRecognitionClient. Otherwise it compiles to a stub that
// reports "not ready", keeping host unit-test builds free of AI dependencies.

#ifndef FOUNDATION_SCREENPARSER_OCR_CORE_VISION_OCR_ENGINE_H
#define FOUNDATION_SCREENPARSER_OCR_CORE_VISION_OCR_ENGINE_H

#include <memory>
#include <string>

#include "i_ocr_engine.h"

namespace OHOS {
namespace ScreenParser {

class CoreVisionOcrEngine : public IOcrEngine {
public:
    CoreVisionOcrEngine();
    ~CoreVisionOcrEngine() override;

    bool Init(std::string &error) override;
    bool IsReady() const override;
    bool Recognize(const uint8_t *rgba, int32_t width, int32_t height,
                   std::vector<OcrTextBlock> &out, std::string &error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_OCR_CORE_VISION_OCR_ENGINE_H
