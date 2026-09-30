/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for the node-tree hierarchy and the on-device OCR fusion pipeline.
// The OCR backend is injected (fake engine / ocr func) so everything runs
// offline without core_vision, mirroring how ScreenAnalyzer is exercised in
// test_analyzer.cpp.

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "core_vision_ocr_engine.h"
#include "i_ocr_engine.h"
#include "i_vlm_engine.h"
#include "json.h"
#include "screen_analyzer.h"
#include "screen_capture.h"
#include "screen_error.h"
#include "screen_schema.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

// ---- helpers -------------------------------------------------------------

ScreenNode MakeNode(const std::string &id, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    ScreenNode node;
    node.id = id;
    node.label = id;
    node.role = "other";
    node.bounds.x1 = x1;
    node.bounds.y1 = y1;
    node.bounds.x2 = x2;
    node.bounds.y2 = y2;
    node.confidence = 0.9;
    node.evidence = "vision";
    return node;
}

const ScreenNode *FindById(const std::vector<ScreenNode> &nodes, const std::string &id) {
    for (const auto &n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

// Fake VLM engine returning a canned response.
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
        outText = response_;
        return true;
    }

private:
    std::string response_;
    bool ready_ = false;
};

// Fake OCR engine returning a fixed block list once Init() succeeds.
class FakeOcrEngine : public IOcrEngine {
public:
    explicit FakeOcrEngine(std::vector<OcrTextBlock> blocks) : blocks_(std::move(blocks)) {}
    bool Init(std::string &) override {
        ready_ = true;
        return true;
    }
    bool IsReady() const override { return ready_; }
    bool Recognize(const uint8_t *, int32_t, int32_t, std::vector<OcrTextBlock> &out,
                   std::string &) override {
        out = blocks_;
        return ready_;
    }

private:
    std::vector<OcrTextBlock> blocks_;
    bool ready_ = false;
};

CapturedScreen MakeScreen(bool sensitive = false) {
    CapturedScreen screen;
    screen.width = 16;
    screen.height = 16;
    screen.isSensitive = sensitive;
    screen.rgba.assign(static_cast<size_t>(16 * 16 * 4), 0);
    for (size_t i = 0; i + 3 < screen.rgba.size(); i += 4) {
        screen.rgba[i] = 120;
        screen.rgba[i + 1] = 200;
        screen.rgba[i + 2] = 80;
        screen.rgba[i + 3] = 255;
    }
    return screen;
}

// ---- geometry helpers ----------------------------------------------------

TEST(NodeTreeGeometryTest, AreaAndContainment) {
    Bounds outer{0, 0, 100, 100};
    Bounds inner{0, 0, 50, 100};
    EXPECT_NEAR(BoundsArea(outer), 10000.0, 1e-9);
    // inner is fully covered by outer -> 1.0
    EXPECT_NEAR(BoundsContainment(inner, outer), 1.0, 1e-9);
    // outer covered by inner -> half.
    EXPECT_NEAR(BoundsContainment(outer, inner), 0.5, 1e-9);

    Bounds disjoint{200, 200, 300, 300};
    EXPECT_NEAR(BoundsContainment(outer, disjoint), 0.0, 1e-9);

    Bounds degenerate{10, 10, 10, 10};
    EXPECT_NEAR(BoundsArea(degenerate), 0.0, 1e-9);
    EXPECT_NEAR(BoundsContainment(degenerate, outer), 0.0, 1e-9);
}

// ---- BuildNodeTree -------------------------------------------------------

TEST(BuildNodeTreeTest, InfersGeometricParentAndDepth) {
    std::vector<ScreenNode> nodes;
    nodes.push_back(MakeNode("root", 0, 0, 1000, 1000));
    nodes.push_back(MakeNode("child1", 100, 100, 400, 300));
    nodes.push_back(MakeNode("child2", 500, 500, 900, 900));
    nodes.push_back(MakeNode("grand", 150, 150, 250, 250));

    BuildNodeTree(nodes);

    const ScreenNode *root = FindById(nodes, "root");
    const ScreenNode *child1 = FindById(nodes, "child1");
    const ScreenNode *child2 = FindById(nodes, "child2");
    const ScreenNode *grand = FindById(nodes, "grand");
    ASSERT_TRUE(root != nullptr && child1 != nullptr && child2 != nullptr && grand != nullptr);

    EXPECT_TRUE(root->parentId.empty());
    EXPECT_EQ(root->depth, 0);
    EXPECT_EQ(child1->parentId, std::string("root"));
    EXPECT_EQ(child1->depth, 1);
    EXPECT_EQ(child2->parentId, std::string("root"));
    EXPECT_EQ(child2->depth, 1);
    // Smallest strictly-larger container of grand is child1, not root.
    EXPECT_EQ(grand->parentId, std::string("child1"));
    EXPECT_EQ(grand->depth, 2);

    // Root children are in reading order (child1 y1=100 before child2 y1=500).
    ASSERT_EQ(root->children.size(), static_cast<size_t>(2));
    EXPECT_EQ(root->children[0], std::string("child1"));
    EXPECT_EQ(root->children[1], std::string("child2"));
    ASSERT_EQ(child1->children.size(), static_cast<size_t>(1));
    EXPECT_EQ(child1->children[0], std::string("grand"));
}

TEST(BuildNodeTreeTest, DisjointNodesAreAllRoots) {
    std::vector<ScreenNode> nodes;
    nodes.push_back(MakeNode("a", 0, 0, 100, 100));
    nodes.push_back(MakeNode("b", 500, 500, 600, 600));
    BuildNodeTree(nodes);
    EXPECT_TRUE(FindById(nodes, "a")->parentId.empty());
    EXPECT_TRUE(FindById(nodes, "b")->parentId.empty());
    EXPECT_EQ(FindById(nodes, "a")->depth, 0);
    EXPECT_EQ(FindById(nodes, "b")->depth, 0);
}

TEST(BuildNodeTreeTest, HonorsExplicitParentWhenValid) {
    std::vector<ScreenNode> nodes;
    ScreenNode header = MakeNode("header", 0, 0, 1000, 120);
    ScreenNode button = MakeNode("button", 40, 40, 200, 100);
    // Geometrically button would be inside header anyway; force an explicit
    // parent to a node that does NOT contain it and confirm it is honored.
    ScreenNode sidebar = MakeNode("sidebar", 900, 200, 1000, 900);
    button.parentId = "sidebar";
    nodes.push_back(header);
    nodes.push_back(button);
    nodes.push_back(sidebar);
    BuildNodeTree(nodes);
    EXPECT_EQ(FindById(nodes, "button")->parentId, std::string("sidebar"));
    EXPECT_EQ(FindById(nodes, "button")->depth, 1);
}

TEST(BuildNodeTreeTest, InvalidExplicitParentFallsBackToGeometry) {
    std::vector<ScreenNode> nodes;
    nodes.push_back(MakeNode("root", 0, 0, 1000, 1000));
    ScreenNode child = MakeNode("child", 100, 100, 300, 300);
    child.parentId = "does-not-exist";
    nodes.push_back(child);
    BuildNodeTree(nodes);
    EXPECT_EQ(FindById(nodes, "child")->parentId, std::string("root"));
}

TEST(BuildNodeTreeTest, SelfParentIsCleared) {
    std::vector<ScreenNode> nodes;
    ScreenNode loop = MakeNode("loop", 0, 0, 100, 100);
    loop.parentId = "loop";  // self reference must be dropped
    nodes.push_back(loop);
    BuildNodeTree(nodes);
    EXPECT_TRUE(FindById(nodes, "loop")->parentId.empty());
}

TEST(BuildNodeTreeTest, EmptyInputIsSafe) {
    std::vector<ScreenNode> nodes;
    BuildNodeTree(nodes);  // must not crash
    EXPECT_EQ(nodes.size(), static_cast<size_t>(0));
    Value tree = BuildNodeTreeJson(nodes);
    EXPECT_TRUE(tree.is_array());
    EXPECT_EQ(tree.size(), static_cast<size_t>(0));
}

TEST(BuildNodeTreeTest, TreeJsonIsNestedFromRoots) {
    std::vector<ScreenNode> nodes;
    nodes.push_back(MakeNode("root", 0, 0, 1000, 1000));
    nodes.push_back(MakeNode("child", 100, 100, 400, 300));
    BuildNodeTree(nodes);

    Value tree = BuildNodeTreeJson(nodes);
    ASSERT_TRUE(tree.is_array());
    ASSERT_EQ(tree.size(), static_cast<size_t>(1));  // only "root" is a root
    const Value &root = tree.items()[static_cast<size_t>(0)];
    EXPECT_EQ(root.at("id").as_string(), std::string("root"));
    ASSERT_TRUE(root.at("children").is_array());
    ASSERT_EQ(root.at("children").size(), static_cast<size_t>(1));
    EXPECT_EQ(root.at("children").items()[static_cast<size_t>(0)].at("id").as_string(),
              std::string("child"));
}

TEST(ScreenNodeJsonTest, ExposesHierarchyAndSourceKeys) {
    ScreenNode node = MakeNode("n", 10, 20, 300, 400);
    node.source = "ocr";
    node.text = "识别文本";
    node.parentId = "p";
    node.depth = 2;
    node.children = {"c1", "c2"};
    Value out = node.ToJson();
    EXPECT_EQ(out.at("source").as_string(), std::string("ocr"));
    EXPECT_EQ(out.at("text").as_string(), std::string("识别文本"));
    EXPECT_EQ(out.at("parent_id").as_string(), std::string("p"));
    EXPECT_EQ(static_cast<int>(out.at("depth").as_int()), 2);
    ASSERT_TRUE(out.at("children").is_array());
    EXPECT_EQ(out.at("children").size(), static_cast<size_t>(2));
}

// ---- OCR fusion ----------------------------------------------------------

OcrTextBlock MakeBlock(const std::string &text, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       float conf = 0.95f) {
    OcrTextBlock block;
    block.text = text;
    block.bounds.x1 = x1;
    block.bounds.y1 = y1;
    block.bounds.x2 = x2;
    block.bounds.y2 = y2;
    block.confidence = conf;
    return block;
}

TEST(OcrTextBlockTest, ToJsonKeys) {
    OcrTextBlock block = MakeBlock("你好", 10, 20, 300, 80, 0.9f);
    Value out = block.ToJson();
    EXPECT_EQ(out.at("text").as_string(), std::string("你好"));
    EXPECT_TRUE(out.at("bounds").is_object());
    EXPECT_EQ(static_cast<int>(out.at("bounds").at("x1").as_int()), 10);
    EXPECT_NEAR(out.at("confidence").as_double(), 0.9, 1e-6);
}

TEST(MergeOcrTest, ReinforcesOverlappingPendingNode) {
    ScreenAnalysis analysis;
    analysis.screenWidth = 1080;
    analysis.screenHeight = 1920;
    ScreenNode wifi = MakeNode("wifi", 40, 100, 960, 180);
    wifi.label = "无线局域网";
    wifi.confidence = 0.7;
    wifi.evidence = "pending";  // >= threshold 0.68 -> OCR upgrades to vision
    analysis.nodes.push_back(wifi);

    std::vector<OcrTextBlock> blocks;
    blocks.push_back(MakeBlock("无线局域网", 50, 110, 950, 170));
    blocks.push_back(MakeBlock("页面底部的独立文本", 100, 1800, 600, 1860, 0.9f));

    ScreenAnalyzer::MergeOcrIntoAnalysis(analysis, blocks, 0.68f);

    EXPECT_EQ(analysis.ocrText, std::string("无线局域网\n页面底部的独立文本"));
    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(2));

    const ScreenNode *fused = FindById(analysis.nodes, "wifi");
    ASSERT_TRUE(fused != nullptr);
    EXPECT_EQ(fused->source, std::string("fused"));
    EXPECT_EQ(fused->text, std::string("无线局域网"));
    EXPECT_EQ(fused->evidence, std::string("vision"));  // upgraded

    // Uncovered block became a standalone OCR text node.
    const ScreenNode *extra = FindById(analysis.nodes, "ocr-1");
    ASSERT_TRUE(extra != nullptr);
    EXPECT_EQ(extra->role, std::string("text"));
    EXPECT_EQ(extra->source, std::string("ocr"));
    EXPECT_EQ(extra->label, std::string("页面底部的独立文本"));
    EXPECT_NEAR(extra->confidence, 0.9, 1e-6);
}

TEST(MergeOcrTest, BumpsConfidenceForVisionNode) {
    ScreenAnalysis analysis;
    ScreenNode node = MakeNode("title", 40, 100, 960, 180);
    node.confidence = 0.9;
    node.evidence = "vision";
    analysis.nodes.push_back(node);

    std::vector<OcrTextBlock> blocks;
    blocks.push_back(MakeBlock("标题", 50, 110, 950, 170));
    ScreenAnalyzer::MergeOcrIntoAnalysis(analysis, blocks, 0.68f);

    ASSERT_EQ(analysis.nodes.size(), static_cast<size_t>(1));  // block consumed
    EXPECT_NEAR(analysis.nodes[0].confidence, 0.95, 1e-9);     // +0.05
    EXPECT_EQ(analysis.nodes[0].source, std::string("fused"));
}

TEST(MergeOcrTest, EmptyBlocksAreNoOp) {
    ScreenAnalysis analysis;
    analysis.nodes.push_back(MakeNode("only", 0, 0, 100, 100));
    ScreenAnalyzer::MergeOcrIntoAnalysis(analysis, {}, 0.68f);
    EXPECT_EQ(analysis.nodes.size(), static_cast<size_t>(1));
    EXPECT_TRUE(analysis.ocrText.empty());
}

TEST(MergeOcrTest, FillsEmptyLabelFromOcr) {
    ScreenAnalysis analysis;
    ScreenNode node = MakeNode("unlabeled", 40, 100, 960, 180);
    node.label = "未命名节点";
    node.confidence = 0.9;
    node.evidence = "vision";
    analysis.nodes.push_back(node);

    std::vector<OcrTextBlock> blocks;
    blocks.push_back(MakeBlock("蓝牙", 50, 110, 950, 170));
    ScreenAnalyzer::MergeOcrIntoAnalysis(analysis, blocks, 0.68f);
    EXPECT_EQ(analysis.nodes[0].label, std::string("蓝牙"));
}

// ---- RecognizeText via analyzer -----------------------------------------

TEST(RecognizeTextTest, ReturnsBlocksAndText) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer(&engine);
    analyzer.SetCaptureFunc([]() { return MakeScreen(false); });
    analyzer.SetOcrFunc([](const CapturedScreen &) {
        return std::vector<OcrTextBlock>{MakeBlock("第一行", 0, 0, 500, 60),
                                         MakeBlock("第二行", 0, 80, 500, 140)};
    });

    Value result = analyzer.RecognizeText();
    EXPECT_EQ(result.at("ocr_text").as_string(), std::string("第一行\n第二行"));
    EXPECT_EQ(result.at("blocks").size(), static_cast<size_t>(2));
    EXPECT_EQ(static_cast<int>(result.at("screen_width").as_int()), 16);
    EXPECT_EQ(result.at("device_id").as_string(), std::string("local"));
}

TEST(RecognizeTextTest, UsesInjectedOcrEngine) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    FakeOcrEngine ocr({MakeBlock("引擎文本", 0, 0, 400, 60)});
    std::string initErr;
    ocr.Init(initErr);
    ScreenAnalyzer analyzer(&engine, {}, &ocr);
    analyzer.SetCaptureFunc([]() { return MakeScreen(false); });

    Value result = analyzer.RecognizeText();
    EXPECT_EQ(result.at("ocr_text").as_string(), std::string("引擎文本"));
    EXPECT_EQ(result.at("blocks").size(), static_cast<size_t>(1));
}

TEST(RecognizeTextTest, SensitivePageThrowsInputError) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer(&engine);
    analyzer.SetCaptureFunc([]() { return MakeScreen(true); });
    analyzer.SetOcrFunc([](const CapturedScreen &) { return std::vector<OcrTextBlock>{}; });
    EXPECT_THROW(analyzer.RecognizeText(), ScreenInputError);
}

TEST(RecognizeTextTest, NoOcrBackendThrowsModelError) {
    FakeEngine engine("{}");
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer(&engine);  // no ocr func, no ocr engine
    analyzer.SetCaptureFunc([]() { return MakeScreen(false); });
    EXPECT_THROW(analyzer.RecognizeText(), ScreenModelError);
}

// ---- Analyze integration -------------------------------------------------

TEST(AnalyzeOcrTest, FusesOcrAndBuildsTree) {
    const std::string response = R"({
        "page": {"title": "设置", "page_type": "settings", "summary": "系统设置"},
        "nodes": [
            {"id": "container", "label": "列表", "role": "list_item",
             "bounds": [0, 0, 1000, 1000], "confidence": 0.9, "evidence": "vision"},
            {"id": "wifi", "label": "无线局域网", "role": "list_item",
             "bounds": [40, 100, 960, 180], "confidence": 0.9, "evidence": "vision"}
        ]
    })";
    FakeEngine engine(response);
    std::string err;
    engine.Load(EngineConfig{}, err);
    ScreenAnalyzer analyzer(&engine);
    analyzer.SetCaptureFunc([]() { return MakeScreen(false); });
    analyzer.SetCurrentAppFunc([]() { return std::string("设置"); });
    analyzer.SetPreviewFunc([](const CapturedScreen &) { return std::string("data:image/png;base64,AA=="); });
    analyzer.SetOcrFunc([](const CapturedScreen &) {
        return std::vector<OcrTextBlock>{MakeBlock("无线局域网", 50, 110, 950, 170),
                                         MakeBlock("底部说明", 100, 900, 600, 960)};
    });

    Value result = analyzer.Analyze();
    EXPECT_TRUE(result.at("ocr_enabled").as_bool());
    EXPECT_EQ(result.at("ocr").size(), static_cast<size_t>(2));
    EXPECT_EQ(result.at("ocr_text").as_string(), std::string("无线局域网\n底部说明"));

    // Node tree present: only "container" should be a root.
    ASSERT_TRUE(result.at("tree").is_array());
    EXPECT_EQ(result.at("tree").size(), static_cast<size_t>(1));
    EXPECT_EQ(result.at("tree").items()[static_cast<size_t>(0)].at("id").as_string(),
              std::string("container"));
}

TEST(StatusTest, CapabilitiesReflectOcrAndTree) {
    FakeEngine engine("{}");
    ScreenAnalyzer analyzer(&engine);  // not loaded -> model_ready false
    analyzer.SetOcrFunc([](const CapturedScreen &) { return std::vector<OcrTextBlock>{}; });

    Value status = analyzer.Status();
    EXPECT_TRUE(status.at("ocr_ready").as_bool());
    ASSERT_TRUE(status.at("capabilities").is_array());
    bool hasTree = false;
    bool hasOcr = false;
    const json::Array &caps = status.at("capabilities").items();
    for (const auto &c : caps) {
        if (c.as_string() == "node_tree") {
            hasTree = true;
        }
        if (c.as_string() == "ocr") {
            hasOcr = true;
        }
    }
    EXPECT_TRUE(hasTree);
    EXPECT_TRUE(hasOcr);
}

TEST(StatusTest, OcrDisabledHidesCapability) {
    FakeEngine engine("{}");
    ScreenAnalyzerConfig config;
    config.enableOcr = false;
    ScreenAnalyzer analyzer(&engine, config);
    analyzer.SetOcrFunc([](const CapturedScreen &) { return std::vector<OcrTextBlock>{}; });
    Value status = analyzer.Status();
    EXPECT_FALSE(status.at("ocr_ready").as_bool());
}

// ---- Host stub behaviour -------------------------------------------------

TEST(CoreVisionStubTest, IsNotReadyWithoutBackend) {
    // In the host build (SCREENPARSER_ENABLE_OCR undefined) the engine is a
    // stub that reports "not ready" instead of touching core_vision.
    CoreVisionOcrEngine engine;
#if defined(SCREENPARSER_ENABLE_OCR)
    // On-device build: instance may or may not be available; just ensure the
    // call does not crash and returns a boolean.
    std::string err;
    (void)engine.Init(err);
    (void)engine.IsReady();
#else
    EXPECT_FALSE(engine.IsReady());
    std::string err;
    EXPECT_FALSE(engine.Init(err));
    EXPECT_FALSE(err.empty());
    std::vector<OcrTextBlock> out;
    EXPECT_FALSE(engine.Recognize(nullptr, 0, 0, out, err));
    EXPECT_TRUE(out.empty());
#endif
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
