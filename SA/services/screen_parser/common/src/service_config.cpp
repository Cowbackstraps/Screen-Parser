/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "service_config.h"

#include <fstream>
#include <sstream>

namespace OHOS {
namespace ScreenParser {

namespace {

// Each reader only overwrites `out` when the key is present with the expected
// JSON type, so a partial config keeps the built-in defaults for other keys.
void ReadString(const json::Value &obj, const char *key, std::string &out) {
    const json::Value &v = obj.at(key);
    if (v.is_string()) {
        out = v.as_string();
    }
}

void ReadInt(const json::Value &obj, const char *key, int32_t &out) {
    const json::Value &v = obj.at(key);
    if (v.is_number()) {
        out = static_cast<int32_t>(v.as_int(static_cast<int64_t>(out)));
    }
}

void ReadFloat(const json::Value &obj, const char *key, float &out) {
    const json::Value &v = obj.at(key);
    if (v.is_number()) {
        out = static_cast<float>(v.as_double(static_cast<double>(out)));
    }
}

void ReadBool(const json::Value &obj, const char *key, bool &out) {
    const json::Value &v = obj.at(key);
    if (v.is_bool()) {
        out = v.as_bool();
    }
}

std::string LowerAscii(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

}  // namespace

void ServiceConfig::ApplyDefaults() {
    if (backend.empty()) {
        backend = "mslite";
    }
    if (modelRoot.empty()) {
        modelRoot = "/system/etc/screenparser";
    }
    if (visionModelPath.empty()) {
        visionModelPath = modelRoot + "/qwen2vl_vision.ms";
    }
    if (languageModelPath.empty()) {
        languageModelPath = modelRoot + "/qwen2vl_lm.ms";
    }
    if (vocabPath.empty()) {
        vocabPath = modelRoot + "/vocab.json";
    }
    if (mergesPath.empty()) {
        mergesPath = modelRoot + "/merges.txt";
    }
    if (imageInputSize <= 0) {
        imageInputSize = 448;
    }
    if (threadNum <= 0) {
        threadNum = 4;
    }
    if (maxTokens <= 0) {
        maxTokens = 4000;
    }
    if (timeoutMs <= 0) {
        timeoutMs = 30000;
    }
    if (maxNodes <= 0) {
        maxNodes = 300;
    }
    if (http.host.empty()) {
        http.host = "127.0.0.1";
    }
    if (http.port <= 0 || http.port > 65535) {
        http.port = 8765;
    }
}

ServiceConfig ServiceConfig::FromJson(const json::Value &obj, ServiceConfig base) {
    if (!obj.is_object()) {
        base.ApplyDefaults();
        return base;
    }

    ReadString(obj, "backend", base.backend);
    ReadString(obj, "model_root", base.modelRoot);
    ReadString(obj, "vision_model_path", base.visionModelPath);
    ReadString(obj, "language_model_path", base.languageModelPath);
    ReadString(obj, "vocab_path", base.vocabPath);
    ReadString(obj, "merges_path", base.mergesPath);
    ReadInt(obj, "image_input_size", base.imageInputSize);
    ReadInt(obj, "thread_num", base.threadNum);
    ReadInt(obj, "max_tokens", base.maxTokens);
    ReadFloat(obj, "temperature", base.temperature);

    ReadString(obj, "base_url", base.baseUrl);
    ReadString(obj, "model", base.model);
    ReadString(obj, "api_key", base.apiKey);
    ReadInt(obj, "timeout_ms", base.timeoutMs);

    ReadFloat(obj, "pending_threshold", base.pendingThreshold);
    ReadInt(obj, "max_nodes", base.maxNodes);
    ReadBool(obj, "enable_ocr", base.enableOcr);
    ReadBool(obj, "enable_node_tree", base.enableNodeTree);

    const json::Value &http = obj.at("http");
    if (http.is_object()) {
        ReadBool(http, "enable", base.http.enable);
        ReadString(http, "host", base.http.host);
        ReadInt(http, "port", base.http.port);
        ReadString(http, "static_dir", base.http.staticDir);
    }

    // Normalize the backend spelling; anything unrecognized falls back to the
    // on-device engine so a typo never silently enables a network backend.
    std::string normalized = LowerAscii(base.backend);
    base.backend = (normalized == "remote" || normalized == "openai") ? "remote" : "mslite";

    base.ApplyDefaults();
    return base;
}

ServiceConfig ServiceConfig::FromJson(const json::Value &obj) {
    return FromJson(obj, ServiceConfig{});
}

ServiceConfig ServiceConfig::Parse(const std::string &text, bool &ok) {
    ok = false;
    json::Value parsed;
    std::string error;
    if (!json::Value::Parse(text, parsed, error) || !parsed.is_object()) {
        ServiceConfig cfg;
        cfg.ApplyDefaults();
        return cfg;
    }
    ok = true;
    return FromJson(parsed);
}

ServiceConfig ServiceConfig::LoadFromFile(const std::string &path, bool &ok) {
    ok = false;
    if (path.empty()) {
        ServiceConfig cfg;
        cfg.ApplyDefaults();
        return cfg;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ServiceConfig cfg;
        cfg.ApplyDefaults();
        return cfg;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return Parse(buffer.str(), ok);
}

}  // namespace ScreenParser
}  // namespace OHOS
