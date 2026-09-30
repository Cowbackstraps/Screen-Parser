/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for the schema module: Bounds normalization and the analyzer's
// pure normalization step (ported from screen/tests/test_analyzer.py).

#include <string>

#include "gtest/gtest.h"

#include "json.h"
#include "screen_analyzer.h"
#include "screen_schema.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

Value Parse(const std::string &text) {
    return Value::ParseOrThrow(text);
}

TEST(BoundsTest, NormalizedArrayIsAccepted) {
    Bounds b;
    Value value = Parse("[40, 100, 960, 180]");
    ASSERT_TRUE(Bounds::FromValue(value, 1320, 2856, b));
    EXPECT_EQ(b.x1, 40);
    EXPECT_EQ(b.y1, 100);
    EXPECT_EQ(b.x2, 960);
    EXPECT_EQ(b.y2, 180);
}

TEST(BoundsTest, ObjectFormIsAccepted) {
    Bounds b;
    Value value = Parse("{\"x1\": 10, \"y1\": 20, \"x2\": 300, \"y2\": 400}");
    ASSERT_TRUE(Bounds::FromValue(value, 1080, 1920, b));
    EXPECT_EQ(b.x1, 10);
    EXPECT_EQ(b.y1, 20);
    EXPECT_EQ(b.x2, 300);
    EXPECT_EQ(b.y2, 400);
}

TEST(BoundsTest, PixelCoordinatesAreRescaled) {
    Bounds b;
    // x2/y2 exceed 1000 -> interpreted as pixels and rescaled to 0-1000.
    Value value = Parse("[0, 0, 1320, 2856]");
    ASSERT_TRUE(Bounds::FromValue(value, 1320, 2856, b));
    EXPECT_EQ(b.x1, 0);
    EXPECT_EQ(b.y1, 0);
    EXPECT_EQ(b.x2, 1000);
    EXPECT_EQ(b.y2, 1000);
}

TEST(BoundsTest, CoordinatesAreClampedAndSwapped) {
    Bounds b;
    Value value = Parse("[1200, -50, 10, 5]");  // x1>x2 after clamp, y1<0
    // x2=10, y2=5 are <=1000 so no pixel rescale; x1 clamps to 1000.
    ASSERT_TRUE(Bounds::FromValue(value, 1320, 2856, b));
    EXPECT_EQ(b.x1, 10);
    EXPECT_EQ(b.x2, 1000);
    EXPECT_EQ(b.y1, 0);
    EXPECT_EQ(b.y2, 5);
}

TEST(BoundsTest, DegenerateBoxIsRejected) {
    Bounds b;
    Value value = Parse("[10, 10, 11, 11]");  // side < 2
    EXPECT_FALSE(Bounds::FromValue(value, 1080, 1920, b));
}

TEST(BoundsTest, WrongShapeIsRejected) {
    Bounds b;
    EXPECT_FALSE(Bounds::FromValue(Parse("[1, 2, 3]"), 1080, 1920, b));
    EXPECT_FALSE(Bounds::FromValue(Parse("\"box\""), 1080, 1920, b));
    EXPECT_FALSE(Bounds::FromValue(Parse("null"), 1080, 1920, b));
}

TEST(BoundsTest, PixelRescaleWithoutScreenSizeIsRejected) {
    Bounds b;
    Value value = Parse("[0, 0, 1320, 2856]");
    EXPECT_FALSE(Bounds::FromValue(value, 0, 0, b));
}

TEST(TruncateTest, TruncatesByCharacterNotByte) {
    // Each Chinese char is 3 UTF-8 bytes; truncation must not split them.
    std::string text = "无线局域网设置";  // 7 chars
    EXPECT_EQ(TruncateUtf8(text, 3), std::string("无线局"));
    EXPECT_EQ(TruncateUtf8(text, 100), text);
    EXPECT_EQ(TruncateUtf8("abc", 2), std::string("ab"));
}

TEST(RoundTest, RoundsToThreeDecimals) {
    EXPECT_NEAR(Round3(0.123456), 0.123, 1e-9);
    EXPECT_NEAR(Round3(0.94), 0.94, 1e-9);
    EXPECT_NEAR(Round3(1.0), 1.0, 1e-9);
}

// Ported from test_analyzer.py::test_analyze_returns_normalized_nodes.
TEST(NormalizeAnalysisTest, EvidenceVisionAndPending) {
    Value raw = Parse(R"({
        "page": {"title": "设置", "page_type": "settings", "summary": "系统设置页面"},
        "nodes": [
            {"id": "wifi", "label": "无线局域网", "role": "list_item",
             "bounds": [40, 100, 960, 180], "confidence": 0.94,
             "interactive": true, "evidence": "vision"},
            {"id": "uncertain", "label": "模糊图标", "role": "icon",
             "bounds": [20, 20, 80, 80], "confidence": 0.4, "evidence": "vision"}
        ]
    })");

    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "设置", 1320, 2856, 0.68f, 300);

    EXPECT_EQ(analysis.page.pageType, std::string("settings"));
    EXPECT_EQ(analysis.currentApp, std::string("设置"));
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(2));
    EXPECT_EQ(analysis.nodes[0].id, std::string("wifi"));
    EXPECT_EQ(analysis.nodes[0].evidence, std::string("vision"));
    EXPECT_TRUE(analysis.nodes[0].interactive);
    EXPECT_NEAR(analysis.nodes[0].confidence, 0.94, 1e-9);
    // confidence 0.4 < 0.68 -> downgraded to pending.
    EXPECT_EQ(analysis.nodes[1].evidence, std::string("pending"));
}

TEST(NormalizeAnalysisTest, NonVisionEvidenceIsPending) {
    Value raw = Parse(R"({
        "nodes": [
            {"id": "a", "bounds": [0, 0, 100, 100], "confidence": 0.99,
             "evidence": "heuristic"}
        ]
    })");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 300);
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(1));
    EXPECT_EQ(analysis.nodes[0].evidence, std::string("pending"));
}

TEST(NormalizeAnalysisTest, ConfidenceIsClamped) {
    Value raw = Parse(R"({
        "nodes": [
            {"id": "hi", "bounds": [0, 0, 100, 100], "confidence": 5.0, "evidence": "vision"},
            {"id": "lo", "bounds": [0, 0, 100, 100], "confidence": -1.0, "evidence": "vision"}
        ]
    })");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 300);
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(2));
    EXPECT_NEAR(analysis.nodes[0].confidence, 1.0, 1e-9);
    EXPECT_NEAR(analysis.nodes[1].confidence, 0.0, 1e-9);
    // lo confidence 0 -> pending.
    EXPECT_EQ(analysis.nodes[1].evidence, std::string("pending"));
}

TEST(NormalizeAnalysisTest, DuplicateIdsAreSuffixed) {
    Value raw = Parse(R"({
        "nodes": [
            {"id": "dup", "bounds": [0, 0, 100, 100], "confidence": 0.9, "evidence": "vision"},
            {"id": "dup", "bounds": [0, 200, 100, 300], "confidence": 0.9, "evidence": "vision"}
        ]
    })");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 300);
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(2));
    EXPECT_EQ(analysis.nodes[0].id, std::string("dup"));
    EXPECT_EQ(analysis.nodes[1].id, std::string("dup-2"));
}

TEST(NormalizeAnalysisTest, MissingIdGetsDefaultAndDegenerateBoundsSkipped) {
    Value raw = Parse(R"({
        "nodes": [
            {"bounds": [0, 0, 100, 100], "confidence": 0.9, "evidence": "vision"},
            {"id": "bad", "bounds": [5, 5, 6, 6], "confidence": 0.9, "evidence": "vision"}
        ]
    })");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 300);
    // Second node dropped (degenerate bounds); first gets default id "node-1".
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(1));
    EXPECT_EQ(analysis.nodes[0].id, std::string("node-1"));
    EXPECT_EQ(analysis.nodes[0].label, std::string("未命名节点"));
    EXPECT_EQ(analysis.nodes[0].role, std::string("other"));
}

TEST(NormalizeAnalysisTest, NodeCapIsEnforced) {
    std::string nodes = "[";
    for (int i = 0; i < 10; ++i) {
        if (i > 0) {
            nodes += ",";
        }
        nodes += "{\"id\": \"n" + std::to_string(i) +
                 "\", \"bounds\": [0, 0, 100, 100], \"confidence\": 0.9, \"evidence\": \"vision\"}";
    }
    nodes += "]";
    Value raw = Parse("{\"nodes\": " + nodes + "}");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 3);
    EXPECT_EQ(analysis.nodes.size(), static_cast<size_t>(3));
}

TEST(NormalizeAnalysisTest, PageFieldsAreTruncated) {
    std::string longTitle(250, 'a');
    Value raw = Parse("{\"page\": {\"title\": \"" + longTitle + "\", \"page_type\": \"list\"}}");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "unknown", 1080, 1920, 0.68f, 300);
    EXPECT_EQ(analysis.page.title.size(), static_cast<size_t>(200));
    EXPECT_EQ(analysis.page.pageType, std::string("list"));
}

TEST(ScreenAnalysisTest, ToJsonUsesFrontendKeys) {
    Value raw = Parse(R"({
        "page": {"title": "桌面", "page_type": "launcher", "summary": "主屏"},
        "nodes": [
            {"id": "app", "label": "相机", "role": "icon",
             "bounds": [10, 20, 120, 160], "confidence": 0.88,
             "interactive": true, "evidence": "vision"}
        ]
    })");
    ScreenAnalysis analysis =
        ScreenAnalyzer::NormalizeAnalysis(raw, "桌面", 1080, 1920, 0.68f, 300);
    Value out = analysis.ToJson();

    EXPECT_TRUE(out.contains("page"));
    EXPECT_EQ(out.at("page").at("page_type").as_string(), std::string("launcher"));
    EXPECT_EQ(out.at("current_app").as_string(), std::string("桌面"));
    ASSERT_TRUE(out.at("nodes").is_array());
    const Value &node = out.at("nodes")[static_cast<size_t>(0)];
    // Frontend (app.js) reads bounds as an object with x1/y1/x2/y2.
    EXPECT_TRUE(node.at("bounds").is_object());
    EXPECT_EQ(static_cast<int>(node.at("bounds").at("x1").as_int()), 10);
    EXPECT_EQ(node.at("evidence").as_string(), std::string("vision"));
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
