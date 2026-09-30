/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Service-level configuration for the ScreenParser system ability.
//
// Pure logic (defaults + JSON parse + file load) with no OpenHarmony
// dependency, so it is unit-testable off-device. The SA maps a ServiceConfig
// onto EngineConfig (inference) and ScreenAnalyzerConfig (pipeline), and picks
// the VLM backend: "mslite" (on-device MindSpore Lite) or "remote"
// (OpenAI-compatible /chat/completions endpoint).
//
// Default config file: /system/etc/screenparser/service_config.json
// Every key is optional; missing keys keep their built-in defaults. Example:
// {
//   "backend": "remote",
//   "base_url": "http://127.0.0.1:8000/v1",
//   "model": "qwen2-vl",
//   "api_key": "EMPTY",
//   "timeout_ms": 30000,
//   "model_root": "/system/etc/screenparser",
//   "image_input_size": 448,
//   "thread_num": 4,
//   "max_tokens": 4000,
//   "temperature": 0.0,
//   "pending_threshold": 0.68,
//   "max_nodes": 300,
//   "enable_ocr": true,
//   "enable_node_tree": true,
//   "http": { "enable": true, "host": "127.0.0.1", "port": 8765, "static_dir": "" }
// }

#ifndef FOUNDATION_SCREENPARSER_COMMON_SERVICE_CONFIG_H
#define FOUNDATION_SCREENPARSER_COMMON_SERVICE_CONFIG_H

#include <cstdint>
#include <string>

#include "json.h"

namespace OHOS {
namespace ScreenParser {

struct HttpDebugConfig {
    bool enable = true;
    std::string host = "127.0.0.1";
    int32_t port = 8765;
    std::string staticDir;  // empty -> compiled-in default asset directory
};

struct ServiceConfig {
    // VLM backend: "mslite" (on-device) or "remote" (OpenAI-compatible).
    std::string backend = "mslite";

    // On-device model assets. The *Path fields are derived from modelRoot when
    // left empty (see ApplyDefaults).
    std::string modelRoot = "/system/etc/screenparser";
    std::string visionModelPath;
    std::string languageModelPath;
    std::string vocabPath;
    std::string mergesPath;
    int32_t imageInputSize = 448;
    int32_t threadNum = 4;
    int32_t maxTokens = 4000;
    float temperature = 0.0f;

    // Remote backend (used when backend == "remote").
    std::string baseUrl;
    std::string model;
    std::string apiKey;
    int32_t timeoutMs = 30000;

    // Analysis pipeline.
    float pendingThreshold = 0.68f;
    int32_t maxNodes = 300;
    bool enableOcr = true;
    bool enableNodeTree = true;

    // HTTP debug server.
    HttpDebugConfig http;

    // True when the backend selects the remote endpoint.
    bool IsRemote() const { return backend == "remote"; }

    // Fill derived model paths from modelRoot and clamp non-positive numbers to
    // sane defaults. Idempotent.
    void ApplyDefaults();

    // Overlay a JSON object onto `base`. Unknown keys are ignored, missing
    // keys keep `base`. Always normalizes backend and calls ApplyDefaults()
    // before returning.
    static ServiceConfig FromJson(const json::Value &obj, ServiceConfig base);

    // Convenience overload that starts from the built-in defaults.
    static ServiceConfig FromJson(const json::Value &obj);

    // Parse a JSON text buffer. On failure returns defaults with ok=false.
    static ServiceConfig Parse(const std::string &text, bool &ok);

    // Load from a JSON file. On any failure (missing/invalid) returns defaults
    // with ok=false.
    static ServiceConfig LoadFromFile(const std::string &path, bool &ok);
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_COMMON_SERVICE_CONFIG_H
