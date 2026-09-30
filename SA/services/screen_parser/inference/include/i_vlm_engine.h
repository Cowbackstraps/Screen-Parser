/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Vision-language engine abstraction.
//
// IVlmEngine decouples the analyzer from the concrete inference backend. The
// production implementation is MSLiteEngine (MindSpore Lite, on-device). Tests
// inject a fake engine that returns a canned JSON string, letting the whole
// normalization pipeline be verified offline.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_I_VLM_ENGINE_H
#define FOUNDATION_SCREENPARSER_INFERENCE_I_VLM_ENGINE_H

#include <cstdint>
#include <string>

#include "image_preprocess.h"

namespace OHOS {
namespace ScreenParser {

// Selects which inference backend the service instantiates.
enum class VlmBackend {
    kMsLite = 0,  // on-device MindSpore Lite (default)
    kRemote = 1,  // remote OpenAI-compatible Chat Completions endpoint
};

// Configuration for either backend. kMsLite uses the local .ms assets; kRemote
// uses baseUrl/model/apiKey to call an OpenAI-compatible /chat/completions API
// (the same contract the original Python prototype relied on).
struct EngineConfig {
    VlmBackend backend = VlmBackend::kMsLite;

    // ---- On-device (MindSpore Lite) assets --------------------------------
    std::string visionModelPath;   // vision encoder .ms
    std::string languageModelPath; // language model .ms
    std::string vocabPath;         // vocab.json
    std::string mergesPath;        // merges.txt
    int32_t imageInputSize = 448;  // square vision input resolution
    int32_t threadNum = 4;         // CPU backend threads
    int32_t maxTokens = 4000;
    float temperature = 0.0f;      // greedy decoding by default

    // ---- Remote (OpenAI-compatible) backend -------------------------------
    std::string baseUrl;           // e.g. http://127.0.0.1:8000/v1
    std::string model;             // model id (see /v1/models)
    std::string apiKey;            // bearer token; "EMPTY" when no auth
    int32_t timeoutMs = 30000;     // request / generation timeout
};

struct GenerateRequest {
    std::string systemPrompt;
    std::string userText;
    const ImageTensor *image = nullptr;  // preprocessed vision input (on-device)
    std::string imageDataUri;            // "data:image/png;base64,..." (remote)
    int32_t maxTokens = 4000;
    float temperature = 0.0f;
};

class IVlmEngine {
public:
    virtual ~IVlmEngine() = default;

    // Load model + tokenizer. Returns false (error set) on failure.
    virtual bool Load(const EngineConfig &config, std::string &error) = 0;
    virtual bool IsReady() const = 0;

    // Run one generation and return the raw text produced by the model
    // (may still contain Markdown fences; the caller extracts JSON).
    // Returns false (error set) on inference failure.
    virtual bool Generate(const GenerateRequest &request, std::string &outText,
                          std::string &error) = 0;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_I_VLM_ENGINE_H
