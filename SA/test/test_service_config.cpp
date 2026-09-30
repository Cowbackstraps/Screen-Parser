/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for ServiceConfig: built-in defaults, JSON overlay + backend
// normalization, clamping of non-positive numbers, and graceful fallback to
// defaults on invalid JSON / missing file. Pure logic, no device dependency.

#include <string>

#include "gtest/gtest.h"

#include "json.h"
#include "service_config.h"

namespace OHOS {
namespace ScreenParser {
namespace {

TEST(ServiceConfigTest, DefaultsAreSane) {
    ServiceConfig cfg;
    cfg.ApplyDefaults();
    EXPECT_EQ(cfg.backend, std::string("mslite"));
    EXPECT_FALSE(cfg.IsRemote());
    EXPECT_EQ(cfg.modelRoot, std::string("/system/etc/screenparser"));
    EXPECT_EQ(cfg.visionModelPath, std::string("/system/etc/screenparser/qwen2vl_vision.ms"));
    EXPECT_EQ(cfg.languageModelPath, std::string("/system/etc/screenparser/qwen2vl_lm.ms"));
    EXPECT_EQ(cfg.vocabPath, std::string("/system/etc/screenparser/vocab.json"));
    EXPECT_EQ(cfg.mergesPath, std::string("/system/etc/screenparser/merges.txt"));
    EXPECT_EQ(cfg.imageInputSize, 448);
    EXPECT_EQ(cfg.threadNum, 4);
    EXPECT_EQ(cfg.maxTokens, 4000);
    EXPECT_EQ(cfg.timeoutMs, 30000);
    EXPECT_EQ(cfg.maxNodes, 300);
    EXPECT_TRUE(cfg.enableOcr);
    EXPECT_TRUE(cfg.enableNodeTree);
    EXPECT_NEAR(cfg.pendingThreshold, 0.68f, 1e-5);
    EXPECT_TRUE(cfg.http.enable);
    EXPECT_EQ(cfg.http.host, std::string("127.0.0.1"));
    EXPECT_EQ(cfg.http.port, 8765);
    EXPECT_TRUE(cfg.http.staticDir.empty());
}

TEST(ServiceConfigTest, FromJsonOverridesAndNormalizes) {
    const std::string text = R"({
        "backend": "REMOTE",
        "base_url": "http://10.0.0.5:8000/v1",
        "model": "qwen2-vl-7b",
        "api_key": "KEY",
        "timeout_ms": 5000,
        "max_tokens": 256,
        "thread_num": 2,
        "image_input_size": 336,
        "temperature": 0.5,
        "pending_threshold": 0.5,
        "max_nodes": 42,
        "enable_ocr": false,
        "enable_node_tree": false,
        "http": { "enable": false, "port": 9999, "static_dir": "/data/static" }
    })";
    bool ok = false;
    ServiceConfig cfg = ServiceConfig::Parse(text, ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.backend, std::string("remote"));
    EXPECT_TRUE(cfg.IsRemote());
    EXPECT_EQ(cfg.baseUrl, std::string("http://10.0.0.5:8000/v1"));
    EXPECT_EQ(cfg.model, std::string("qwen2-vl-7b"));
    EXPECT_EQ(cfg.apiKey, std::string("KEY"));
    EXPECT_EQ(cfg.timeoutMs, 5000);
    EXPECT_EQ(cfg.maxTokens, 256);
    EXPECT_EQ(cfg.threadNum, 2);
    EXPECT_EQ(cfg.imageInputSize, 336);
    EXPECT_NEAR(cfg.temperature, 0.5f, 1e-5);
    EXPECT_NEAR(cfg.pendingThreshold, 0.5f, 1e-5);
    EXPECT_EQ(cfg.maxNodes, 42);
    EXPECT_FALSE(cfg.enableOcr);
    EXPECT_FALSE(cfg.enableNodeTree);
    EXPECT_FALSE(cfg.http.enable);
    EXPECT_EQ(cfg.http.port, 9999);
    EXPECT_EQ(cfg.http.staticDir, std::string("/data/static"));
    // Keys absent from the JSON keep their built-in defaults.
    EXPECT_EQ(cfg.http.host, std::string("127.0.0.1"));
    EXPECT_EQ(cfg.visionModelPath, std::string("/system/etc/screenparser/qwen2vl_vision.ms"));
}

TEST(ServiceConfigTest, BackendNormalization) {
    bool ok = false;
    ServiceConfig unknown = ServiceConfig::Parse(R"({"backend": "wat"})", ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(unknown.backend, std::string("mslite"));
    EXPECT_FALSE(unknown.IsRemote());

    ServiceConfig openai = ServiceConfig::Parse(R"({"backend": "OpenAI"})", ok);
    EXPECT_EQ(openai.backend, std::string("remote"));
    EXPECT_TRUE(openai.IsRemote());

    ServiceConfig missing = ServiceConfig::Parse(R"({"model": "m"})", ok);
    EXPECT_EQ(missing.backend, std::string("mslite"));
}

TEST(ServiceConfigTest, InvalidJsonYieldsDefaults) {
    bool ok = true;
    ServiceConfig cfg = ServiceConfig::Parse("{not json", ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(cfg.backend, std::string("mslite"));
    EXPECT_EQ(cfg.http.port, 8765);
    EXPECT_EQ(cfg.maxTokens, 4000);
    EXPECT_EQ(cfg.imageInputSize, 448);
}

TEST(ServiceConfigTest, MissingFileYieldsDefaults) {
    bool ok = true;
    ServiceConfig cfg = ServiceConfig::LoadFromFile("screenparser_missing_config_xyz.json", ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(cfg.backend, std::string("mslite"));
    EXPECT_EQ(cfg.imageInputSize, 448);
    EXPECT_EQ(cfg.http.port, 8765);
}

TEST(ServiceConfigTest, NonPositiveNumbersClamped) {
    bool ok = false;
    ServiceConfig cfg = ServiceConfig::Parse(
        R"({"max_tokens": 0, "thread_num": -2, "image_input_size": 0,
            "timeout_ms": -1, "max_nodes": 0, "http": {"port": 0}})",
        ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.maxTokens, 4000);
    EXPECT_EQ(cfg.threadNum, 4);
    EXPECT_EQ(cfg.imageInputSize, 448);
    EXPECT_EQ(cfg.timeoutMs, 30000);
    EXPECT_EQ(cfg.maxNodes, 300);
    EXPECT_EQ(cfg.http.port, 8765);
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
