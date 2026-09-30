/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// End-to-end tests for ScreenAnalyzer using an injected fake VLM engine and
// synthetic capture, so the whole pipeline runs offline.

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "i_vlm_engine.h"
#include "json.h"
#include "screen_analyzer.h"
#include "screen_capture.h"
#include "screen_error.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

// Fake engine returning a canned model response.
class FakeEngine : public IVlmEngine {
public:
    explicit FakeEngine(std::string response) : response_(std::move(response)) {}

    bool Load(const EngineConfig &, std::string &) override {
        ready_ = true;
        return true;
    }
    bool IsReady() const override { return ready_; }
    bool Generate(const GenerateRequest &, std::string &outText, std::string &error) override {
        if (!ready_) {
            error = "engine not ready";
            return false;
        }
        if (failGenerate_) {
            error = "simulated inference failure";
            return false;
        }
        outText = response_;
        return true;
    }

    void SetFail(bool fail) { failGenerate_ = fail; }

private:
    std::string response_;
    bool ready_ = false;
    bool failGenerate_ = false;
};

// Synthetic 16x16 opaque screen (small keeps preprocessing fast).
CapturedScreen MakeScreen(bool sensitive = false) {
    CapturedScreen screen;
    screen.width = 16;
    screen.height = 16;
    screen.isSensitive = sensitive;
    screen.rgba.assign(static_cast<size_t>(16 * 16 * 4), 0);
    for (size_t i = 0; i + 3 < screen.rgba.size(); i += 4) {
        screen.rgba[i] = 120;      // R
        screen.rgba[i + 1] = 200;  // G
        screen.rgba[i + 2] = 80;   // B
        screen.rgba[i + 3] = 255;  // A
    }
    return screen;
}

ScreenAnalyzer MakeAnalyzer(FakeEngine &engine) {
    ScreenAnalyzer analyzer(&engine);
    analyzer.SetCaptureFunc([]() { return MakeScreen(false); });
    analyzer.SetCurrentAppFunc([]() { return std::string("设置"); });
    analyzer.SetPreviewFunc(
        [](const CapturedScreen &) { return std::string("data:image/png;base64,aW1hZ2U="); });
    return analyzer;
}

TEST(AnalyzerTest, ReturnsNormalizedResult) {
    const std::string response = R"({
        "page": {"title": "设置", "page_type": "settings", "summary": "系统设置页面"},
        "nodes": [
            {"id": "wifi", "label": "无线局域网", "role": "list_item",
             "bounds": [40, 100, 960, 180], "confidence": 0.94,
             "interactive": true, "evidence": "vision"},
            {"id": "uncertain", "label": "模糊图标", "role": "icon",
             "bounds": [20, 20, 80, 80], "confidence": 0.4, "evidence": "vision"}
        ]
    })";
    FakeEngine engine(response);
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer = MakeAnalyzer(engine);

    Value result = analyzer.Analyze();
    EXPECT_EQ(result.at("device_id").as_string(), std::string("local"));
    EXPECT_EQ(result.at("page").at("page_type").as_string(), std::string("settings"));
    EXPECT_EQ(result.at("current_app").as_string(), std::string("设置"));
    ASSERT_TRUE(result.at("nodes").is_array());
    EXPECT_EQ(result.at("nodes")[static_cast<size_t>(0)].at("evidence").as_string(),
              std::string("vision"));
    EXPECT_EQ(result.at("nodes")[static_cast<size_t>(1)].at("evidence").as_string(),
              std::string("pending"));
    EXPECT_EQ(result.at("screenshot").as_string().rfind("data:image/png;base64,", 0),
              static_cast<size_t>(0));
}

TEST(AnalyzerTest, StripsMarkdownFenceFromModelOutput) {
    const std::string response = "```json\n{\"page\": {\"title\": \"桌面\", \"page_type\": "
                                 "\"launcher\"}, \"nodes\": []}\n```";
    FakeEngine engine(response);
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer = MakeAnalyzer(engine);

    Value result = analyzer.Analyze();
    EXPECT_EQ(result.at("page").at("page_type").as_string(), std::string("launcher"));
    EXPECT_EQ(result.at("nodes").size(), static_cast<size_t>(0));
}

TEST(AnalyzerTest, SensitivePageThrowsInputError) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer(&engine);
    analyzer.SetCaptureFunc([]() { return MakeScreen(true); });

    bool caught = false;
    try {
        analyzer.Analyze();
    } catch (const ScreenInputError &e) {
        caught = true;
        EXPECT_EQ(e.code(), ScreenError::kSensitivePage);
        EXPECT_TRUE(e.is_input_error());
    }
    EXPECT_TRUE(caught);
}

TEST(AnalyzerTest, EngineNotReadyThrowsModelError) {
    FakeEngine engine("{}");  // not Load()ed -> IsReady false
    ScreenAnalyzer analyzer = MakeAnalyzer(engine);

    EXPECT_THROW(analyzer.Analyze(), ScreenModelError);
}

TEST(AnalyzerTest, InferenceFailureThrowsModelError) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    engine.SetFail(true);
    ScreenAnalyzer analyzer = MakeAnalyzer(engine);

    bool caught = false;
    try {
        analyzer.Analyze();
    } catch (const ScreenModelError &e) {
        caught = true;
        EXPECT_EQ(e.code(), ScreenError::kInferenceFailed);
    }
    EXPECT_TRUE(caught);
}

TEST(AnalyzerTest, NonJsonOutputThrowsModelError) {
    FakeEngine engine("I cannot analyze this image.");
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer = MakeAnalyzer(engine);

    bool caught = false;
    try {
        analyzer.Analyze();
    } catch (const ScreenModelError &e) {
        caught = true;
        EXPECT_EQ(e.code(), ScreenError::kNoJsonObject);
    }
    EXPECT_TRUE(caught);
}

TEST(AnalyzerTest, StatusReflectsReadiness) {
    FakeEngine notReady("{}");
    ScreenAnalyzer analyzer(&notReady);
    Value status = analyzer.Status();
    EXPECT_FALSE(status.at("model_ready").as_bool());
    EXPECT_EQ(status.at("devices").size(), static_cast<size_t>(0));

    std::string err;
    notReady.Load(EngineConfig{}, err);
    Value ready = analyzer.Status();
    EXPECT_TRUE(ready.at("model_ready").as_bool());
    EXPECT_EQ(ready.at("default_device").as_string(), std::string("local"));
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
