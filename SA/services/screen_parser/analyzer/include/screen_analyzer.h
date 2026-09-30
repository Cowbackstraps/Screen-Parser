/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// One-shot semantic analysis for the current OpenHarmony screen.
// Direct port of screen/analyzer.py: assemble the system prompt, run the VLM
// engine on the captured screenshot, extract JSON and normalize it into the
// ScreenAnalysis schema (id de-duplication, confidence clamping, pending
// threshold, node cap, string truncation).

#ifndef FOUNDATION_SCREENPARSER_ANALYZER_SCREEN_ANALYZER_H
#define FOUNDATION_SCREENPARSER_ANALYZER_SCREEN_ANALYZER_H

#include <functional>
#include <string>
#include <vector>

#include "i_ocr_engine.h"
#include "i_vlm_engine.h"
#include "json.h"
#include "screen_capture.h"
#include "screen_schema.h"

namespace OHOS {
namespace ScreenParser {

// The Chinese system prompt, identical in intent to analyzer.py SYSTEM_PROMPT.
extern const char *const kSystemPrompt;

struct ScreenAnalyzerConfig {
    float pendingThreshold = 0.68f;  // SCREEN_PENDING_THRESHOLD
    size_t maxNodes = 300;
    bool enableOcr = true;       // fuse on-device OCR text into the analysis
    bool enableNodeTree = true;  // build the geometric node hierarchy
    EngineConfig engine;
};

// Injectable dependencies so the whole pipeline can be exercised offline.
using CaptureFunc = std::function<CapturedScreen()>;
using CurrentAppFunc = std::function<std::string()>;
using PreviewFunc = std::function<std::string(const CapturedScreen &)>;
using OcrFunc = std::function<std::vector<OcrTextBlock>(const CapturedScreen &)>;

class ScreenAnalyzer {
public:
    // engine is borrowed (not owned) and must outlive the analyzer. ocrEngine
    // is optional (may be null) and likewise borrowed.
    explicit ScreenAnalyzer(IVlmEngine *engine, ScreenAnalyzerConfig config = {},
                            IOcrEngine *ocrEngine = nullptr);

    // Override the capture / app / preview / ocr providers (defaults hit device).
    void SetCaptureFunc(CaptureFunc fn) { capture_ = std::move(fn); }
    void SetCurrentAppFunc(CurrentAppFunc fn) { currentApp_ = std::move(fn); }
    void SetPreviewFunc(PreviewFunc fn) { preview_ = std::move(fn); }
    void SetOcrFunc(OcrFunc fn) { ocr_ = std::move(fn); }
    void SetOcrEngine(IOcrEngine *engine) { ocrEngine_ = engine; }

    const ScreenAnalyzerConfig &config() const { return config_; }

    // Full pipeline: capture -> preprocess -> generate -> parse -> normalize
    // -> (optional) OCR fusion -> (optional) node tree.
    // Throws ScreenInputError / ScreenModelError on failure.
    json::Value Analyze();

    // Text-only recognition: capture -> OCR. Independent of the VLM. Returns
    // { ocr_text, blocks[], screen_width, screen_height, device_id }.
    // Throws ScreenInputError on capture failure, ScreenModelError when OCR is
    // unavailable.
    json::Value RecognizeText();

    // Status payload (model / OCR readiness and capabilities).
    json::Value Status() const;

    // Pure normalization step, exposed for unit tests. Ports _normalize_analysis.
    static ScreenAnalysis NormalizeAnalysis(const json::Value &raw, const std::string &currentApp,
                                            int32_t screenWidth, int32_t screenHeight,
                                            float pendingThreshold, size_t maxNodes);

    // Fuse OCR text blocks into a normalized analysis (unit-testable):
    //  * joins all block text into analysis.ocrText;
    //  * reinforces VLM nodes overlapped by OCR (fills empty labels, records
    //    node.text, corroborates evidence/confidence);
    //  * promotes uncovered OCR blocks into standalone role="text" nodes.
    static void MergeOcrIntoAnalysis(ScreenAnalysis &analysis,
                                     const std::vector<OcrTextBlock> &blocks,
                                     float pendingThreshold);

private:
    IVlmEngine *engine_;
    IOcrEngine *ocrEngine_;
    ScreenAnalyzerConfig config_;
    CaptureFunc capture_;
    CurrentAppFunc currentApp_;
    PreviewFunc preview_;
    OcrFunc ocr_;
};

// Round a double to 3 decimal places (mirrors Python round(x, 3)).
double Round3(double value);

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_ANALYZER_SCREEN_ANALYZER_H
