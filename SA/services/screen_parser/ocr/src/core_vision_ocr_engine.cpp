/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "core_vision_ocr_engine.h"

#include <algorithm>
#include <cmath>

#if defined(SCREENPARSER_ENABLE_OCR)
#include "text_recognition_client.h"
#endif

namespace OHOS {
namespace ScreenParser {

// ---- Pure logic (always compiled) ----------------------------------------

json::Value OcrTextBlock::ToJson() const {
    json::Value obj = json::Value::MakeObject();
    obj.set("text", json::Value(text));
    obj.set("bounds", bounds.ToJson());
    obj.set("confidence", json::Value(static_cast<double>(confidence)));
    return obj;
}

#if defined(SCREENPARSER_ENABLE_OCR)

// ---- On-device backend (core_vision TextRecognition) ---------------------

struct CoreVisionOcrEngine::Impl {
    std::shared_ptr<AIM::TextRecognitionClient> client;
};

namespace {

// Normalize a pixel coordinate to the 0-1000 space.
int32_t NormalizeCoord(double pixel, int32_t total) {
    if (total <= 0) {
        return 0;
    }
    long long v = std::llround(pixel * 1000.0 / static_cast<double>(total));
    if (v < 0) {
        v = 0;
    }
    if (v > 1000) {
        v = 1000;
    }
    return static_cast<int32_t>(v);
}

}  // namespace

CoreVisionOcrEngine::CoreVisionOcrEngine() = default;
CoreVisionOcrEngine::~CoreVisionOcrEngine() = default;

bool CoreVisionOcrEngine::Init(std::string &error) {
    impl_ = std::make_unique<Impl>();
    impl_->client = AIM::TextRecognitionClient::GetInstance();
    if (impl_->client == nullptr) {
        error = "无法获取 core_vision 文字识别实例";
        ready_ = false;
        return false;
    }
    ready_ = true;
    return true;
}

bool CoreVisionOcrEngine::IsReady() const {
    return ready_;
}

bool CoreVisionOcrEngine::Recognize(const uint8_t *rgba, int32_t width, int32_t height,
                                    std::vector<OcrTextBlock> &out, std::string &error) {
    out.clear();
    if (!ready_ || impl_ == nullptr || impl_->client == nullptr) {
        error = "OCR 未就绪";
        return false;
    }
    if (rgba == nullptr || width <= 0 || height <= 0) {
        error = "无效图像输入";
        return false;
    }

    AIM::VisionInfo input;
    input.data = const_cast<uint8_t *>(rgba);
    input.width = width;
    input.height = height;
    input.pixelFormat = AIM::VisionFormat::PIXEL_FORMAT_RGBA_8888;
    input.rotation = AIM::ROTATE_0;
    input.requestId = 1;

    AIM::TextResults results;
    // isKeepSource=false: we only need the recognized text, not the source image.
    int32_t status = impl_->client->AnalyseText(input, results, false);
    if (status != AIM::SUCCESS) {
        error = "文字识别失败, code=" + std::to_string(status);
        return false;
    }

    for (const auto &block : results.textBlocks) {
        if (block.value.empty()) {
            continue;
        }
        OcrTextBlock text;
        text.text = block.value;
        text.bounds.x1 = NormalizeCoord(block.boundingBox.left, width);
        text.bounds.y1 = NormalizeCoord(block.boundingBox.top, height);
        text.bounds.x2 = NormalizeCoord(block.boundingBox.right, width);
        text.bounds.y2 = NormalizeCoord(block.boundingBox.bottom, height);
        // core_vision does not expose a per-block score on all releases; OCR
        // hits are treated as high-confidence visual evidence.
        text.confidence = 0.95f;
        out.push_back(std::move(text));
    }
    return true;
}

#else  // !SCREENPARSER_ENABLE_OCR

// ---- Host stub -----------------------------------------------------------

struct CoreVisionOcrEngine::Impl {};

CoreVisionOcrEngine::CoreVisionOcrEngine() = default;
CoreVisionOcrEngine::~CoreVisionOcrEngine() = default;

bool CoreVisionOcrEngine::Init(std::string &error) {
    error = "OCR 能力未在此构建中启用（SCREENPARSER_ENABLE_OCR）";
    ready_ = false;
    return false;
}

bool CoreVisionOcrEngine::IsReady() const {
    return false;
}

bool CoreVisionOcrEngine::Recognize(const uint8_t *, int32_t, int32_t,
                                    std::vector<OcrTextBlock> &out, std::string &error) {
    out.clear();
    error = "OCR 能力未在此构建中启用（SCREENPARSER_ENABLE_OCR）";
    return false;
}

#endif  // SCREENPARSER_ENABLE_OCR

}  // namespace ScreenParser
}  // namespace OHOS
