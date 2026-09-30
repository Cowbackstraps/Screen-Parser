/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_analyzer.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "image_preprocess.h"
#include "json_constraint_decoder.h"
#include "screen_error.h"

namespace OHOS {
namespace ScreenParser {

const char *const kSystemPrompt =
    "你是鸿蒙系统界面语义解析器。分析给定截图并只返回一个 JSON 对象，不要输出 Markdown 或解释。\n"
    "\n"
    "JSON 格式：\n"
    "{\n"
    "  \"page\": {\n"
    "    \"title\": \"页面标题\",\n"
    "    \"page_type\": \"launcher|settings|list|detail|dialog|form|media|unknown\",\n"
    "    \"summary\": \"一句话描述页面用途\"\n"
    "  },\n"
    "  \"nodes\": [\n"
    "    {\n"
    "      \"id\": \"node-1\",\n"
    "      \"label\": \"屏幕上可见的文字或简短语义名称\",\n"
    "      \"role\": \"button|text|input|icon|image|list_item|switch|tab|navigation|dialog|other\",\n"
    "      \"bounds\": [x1, y1, x2, y2],\n"
    "      \"confidence\": 0.0,\n"
    "      \"interactive\": true,\n"
    "      \"evidence\": \"vision|pending\",\n"
    "      \"description\": \"节点用途\"\n"
    "    }\n"
    "  ]\n"
    "}\n"
    "\n"
    "规则：\n"
    "1. bounds 使用 0-1000 归一化坐标，原点在左上角，边框必须紧贴元素。\n"
    "2. 优先识别可交互控件、页面标题、列表项、输入框及关键状态文本。\n"
    "3. 不要把多个独立控件合并成一个大框，不要输出屏幕外节点。\n"
    "4. 仅凭截图能够直接确认的节点 evidence=vision；被遮挡、边界模糊或语义不确定的节点 "
    "evidence=pending。\n"
    "5. 无法确认时降低 confidence，严禁猜测不存在的控件。\n";

namespace {

// Mimic Python str(value) for the scalar JSON types we care about.
std::string JsonToString(const json::Value &value, const std::string &fallback) {
    if (value.is_string()) {
        return value.as_string();
    }
    if (value.is_number()) {
        double d = value.as_double();
        if (d == std::floor(d) && std::fabs(d) < 1e15) {
            return std::to_string(static_cast<long long>(d));
        }
        return std::to_string(d);
    }
    if (value.is_bool()) {
        return value.as_bool() ? "true" : "false";
    }
    return fallback;
}

std::string LowerAscii(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string Basename(const std::string &path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Obtain OCR blocks from the injected function (tests) or the on-device engine.
bool CollectOcr(const OcrFunc &ocrFn, IOcrEngine *engine, bool enabled,
                const CapturedScreen &screen, std::vector<OcrTextBlock> &out, std::string &error) {
    out.clear();
    if (!enabled) {
        error = "OCR 已禁用";
        return false;
    }
    if (ocrFn) {
        out = ocrFn(screen);
        return true;
    }
    if (engine != nullptr && engine->IsReady()) {
        return engine->Recognize(screen.rgba.data(), screen.width, screen.height, out, error);
    }
    error = "OCR 引擎未就绪";
    return false;
}

}  // namespace

double Round3(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

ScreenAnalyzer::ScreenAnalyzer(IVlmEngine *engine, ScreenAnalyzerConfig config, IOcrEngine *ocrEngine)
    : engine_(engine), ocrEngine_(ocrEngine), config_(std::move(config)) {}

ScreenAnalysis ScreenAnalyzer::NormalizeAnalysis(const json::Value &raw, const std::string &currentApp,
                                                 int32_t screenWidth, int32_t screenHeight,
                                                 float pendingThreshold, size_t maxNodes) {
    ScreenAnalysis analysis;
    analysis.currentApp = currentApp;
    analysis.screenWidth = screenWidth;
    analysis.screenHeight = screenHeight;

    const json::Value &rawPage = raw.is_object() ? raw.at("page") : json::Value::Null();
    if (rawPage.is_object()) {
        analysis.page.title = TruncateUtf8(JsonToString(rawPage.at("title"), ""), 200);
        analysis.page.pageType = TruncateUtf8(JsonToString(rawPage.at("page_type"), "unknown"), 80);
        analysis.page.summary = TruncateUtf8(JsonToString(rawPage.at("summary"), ""), 500);
    }

    const json::Value &rawNodes = raw.is_object() ? raw.at("nodes") : json::Value::Null();
    if (!rawNodes.is_array()) {
        return analysis;
    }

    std::set<std::string> usedIds;
    const json::Array &items = rawNodes.items();
    size_t limit = std::min(items.size(), maxNodes);
    for (size_t i = 0; i < limit; ++i) {
        const json::Value &item = items[i];
        int index = static_cast<int>(i) + 1;
        if (!item.is_object()) {
            continue;
        }

        Bounds bounds;
        if (!Bounds::FromValue(item.at("bounds"), screenWidth, screenHeight, bounds)) {
            continue;
        }

        // id: str(item.get("id") or f"node-{index}")[:80], de-duplicated.
        std::string nodeId;
        const json::Value &idValue = item.at("id");
        if (idValue.is_string() && !idValue.as_string().empty()) {
            nodeId = TruncateUtf8(idValue.as_string(), 80);
        } else {
            nodeId = "node-" + std::to_string(index);
        }
        if (usedIds.count(nodeId) > 0) {
            nodeId = nodeId + "-" + std::to_string(index);
        }
        usedIds.insert(nodeId);

        double confidence = ClampDouble(item.at("confidence").as_double(0.0), 0.0, 1.0);

        std::string evidence = LowerAscii(JsonToString(item.at("evidence"), "vision"));
        if (evidence != "vision" || confidence < static_cast<double>(pendingThreshold)) {
            evidence = "pending";
        }

        ScreenNode node;
        node.id = nodeId;
        node.label = TruncateUtf8(JsonToString(item.at("label"), "未命名节点"), 200);
        node.role = TruncateUtf8(JsonToString(item.at("role"), "other"), 80);
        node.bounds = bounds;
        node.confidence = Round3(confidence);
        node.interactive = item.at("interactive").as_bool(false);
        node.evidence = evidence;
        node.description = TruncateUtf8(JsonToString(item.at("description"), ""), 500);
        analysis.nodes.push_back(std::move(node));
    }
    return analysis;
}

void ScreenAnalyzer::MergeOcrIntoAnalysis(ScreenAnalysis &analysis,
                                          const std::vector<OcrTextBlock> &blocks,
                                          float pendingThreshold) {
    if (blocks.empty()) {
        return;
    }

    // 1. Join all recognized text (reading order as returned by the engine).
    std::string full;
    for (const auto &block : blocks) {
        if (!full.empty()) {
            full += "\n";
        }
        full += block.text;
    }
    analysis.ocrText = full;

    // 2. Reinforce VLM nodes that are corroborated by an OCR block.
    std::vector<bool> consumed(blocks.size(), false);
    constexpr double kReinforceContainment = 0.6;
    for (auto &node : analysis.nodes) {
        size_t best = static_cast<size_t>(-1);
        double bestCoverage = 0.0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            // How much of the OCR block lies inside the node box.
            double coverage = BoundsContainment(blocks[i].bounds, node.bounds);
            if (coverage > bestCoverage) {
                bestCoverage = coverage;
                best = i;
            }
        }
        if (best == static_cast<size_t>(-1) || bestCoverage < kReinforceContainment) {
            continue;
        }
        consumed[best] = true;
        const std::string &ocrText = blocks[best].text;
        node.text = ocrText;
        if (node.label.empty() || node.label == "未命名节点") {
            node.label = TruncateUtf8(ocrText, 200);
        }
        node.source = "fused";
        if (node.evidence == "pending" && node.confidence >= static_cast<double>(pendingThreshold)) {
            // OCR text confirms the region -> upgrade to vision.
            node.evidence = "vision";
        } else if (node.evidence == "vision") {
            node.confidence = Round3(std::min(1.0, node.confidence + 0.05));
        }
    }

    // 3. Uncovered OCR blocks become standalone text nodes.
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (consumed[i]) {
            continue;
        }
        const OcrTextBlock &block = blocks[i];
        if (block.text.empty() || BoundsArea(block.bounds) <= 0.0) {
            continue;
        }
        ScreenNode node;
        node.id = "ocr-" + std::to_string(i);
        node.label = TruncateUtf8(block.text, 200);
        node.text = block.text;
        node.role = "text";
        node.bounds = block.bounds;
        node.confidence = Round3(ClampDouble(static_cast<double>(block.confidence), 0.0, 1.0));
        node.interactive = false;
        node.evidence = "vision";
        node.source = "ocr";
        analysis.nodes.push_back(std::move(node));
    }
}

json::Value ScreenAnalyzer::Analyze() {
    CapturedScreen screen = capture_ ? capture_() : ScreenCapture::Capture();
    if (screen.isSensitive) {
        throw ScreenInputError(ScreenError::kSensitivePage, "当前页面禁止截图，无法进行视觉解析");
    }
    if (screen.rgba.empty() || screen.width <= 0 || screen.height <= 0) {
        throw ScreenInputError(ScreenError::kCaptureFailed, "屏幕截图为空");
    }

    std::string currentApp = currentApp_ ? currentApp_() : std::string("unknown");

    // RGBA -> RGB -> normalized CHW tensor at the model input resolution.
    size_t pixelCount = static_cast<size_t>(screen.width) * screen.height;
    std::vector<uint8_t> rgb = ScreenCapture::RgbaToRgb(screen.rgba.data(), pixelCount);
    int32_t inputSize = config_.engine.imageInputSize;
    NormalizeParams params;
    ImageTensor tensor =
        ResizeAndNormalize(rgb.data(), screen.width, screen.height, inputSize, inputSize, params);

    if (engine_ == nullptr || !engine_->IsReady()) {
        throw ScreenModelError(ScreenError::kModelNotReady, "视觉模型尚未就绪");
    }

    // Encode the preview once. The remote backend needs it as an image data URI
    // inside the request; the web front-end reuses the same string as the
    // response screenshot. The on-device backend ignores imageDataUri and uses
    // the preprocessed tensor instead.
    std::string preview = preview_ ? preview_(screen) : ScreenCapture::EncodePngDataUri(screen);

    GenerateRequest request;
    request.systemPrompt = kSystemPrompt;
    request.userText = "当前前台应用：" + currentApp + "。请解析页面结构、可见语义节点和精确边界。";
    request.image = &tensor;
    request.imageDataUri = preview;
    request.maxTokens = config_.engine.maxTokens;
    request.temperature = config_.engine.temperature;

    std::string text;
    std::string error;
    if (!engine_->Generate(request, text, error)) {
        throw ScreenModelError(ScreenError::kInferenceFailed, "屏幕解析失败: " + error);
    }

    json::Value raw;
    if (!ExtractJsonObject(text, raw, error)) {
        ScreenError code = error.find("no JSON") != std::string::npos ? ScreenError::kNoJsonObject
                                                                    : ScreenError::kJsonParseFailed;
        throw ScreenModelError(code, error);
    }

    ScreenAnalysis analysis = NormalizeAnalysis(raw, currentApp, screen.width, screen.height,
                                                config_.pendingThreshold, config_.maxNodes);

    // Fuse on-device OCR text, then rebuild the node hierarchy.
    std::vector<OcrTextBlock> blocks;
    std::string ocrError;
    bool haveOcr = CollectOcr(ocr_, ocrEngine_, config_.enableOcr, screen, blocks, ocrError);
    if (haveOcr) {
        MergeOcrIntoAnalysis(analysis, blocks, config_.pendingThreshold);
        if (analysis.nodes.size() > config_.maxNodes) {
            analysis.nodes.resize(config_.maxNodes);
        }
    }
    if (config_.enableNodeTree) {
        BuildNodeTree(analysis.nodes);
    }

    json::Value result = analysis.ToJson();
    result.set("device_id", json::Value("local"));
    result.set("screenshot", json::Value(preview));

    json::Value ocrArray = json::Value::MakeArray();
    for (const auto &block : blocks) {
        ocrArray.push_back(block.ToJson());
    }
    result.set("ocr", ocrArray);
    result.set("ocr_enabled", json::Value(haveOcr));
    return result;
}

json::Value ScreenAnalyzer::RecognizeText() {
    CapturedScreen screen = capture_ ? capture_() : ScreenCapture::Capture();
    if (screen.isSensitive) {
        throw ScreenInputError(ScreenError::kSensitivePage, "当前页面禁止截图，无法进行文字识别");
    }
    if (screen.rgba.empty() || screen.width <= 0 || screen.height <= 0) {
        throw ScreenInputError(ScreenError::kCaptureFailed, "屏幕截图为空");
    }

    std::vector<OcrTextBlock> blocks;
    std::string ocrError;
    // RecognizeText is explicitly an OCR request, so ignore the analyze-time
    // enableOcr switch and always attempt recognition.
    if (!CollectOcr(ocr_, ocrEngine_, true, screen, blocks, ocrError)) {
        throw ScreenModelError(ScreenError::kModelNotReady, "文字识别不可用: " + ocrError);
    }

    std::string full;
    json::Value array = json::Value::MakeArray();
    for (const auto &block : blocks) {
        if (!full.empty()) {
            full += "\n";
        }
        full += block.text;
        array.push_back(block.ToJson());
    }

    json::Value result = json::Value::MakeObject();
    result.set("ocr_text", json::Value(full));
    result.set("blocks", array);
    result.set("screen_width", json::Value(screen.width));
    result.set("screen_height", json::Value(screen.height));
    result.set("device_id", json::Value("local"));
    return result;
}

json::Value ScreenAnalyzer::Status() const {
    bool ready = engine_ != nullptr && engine_->IsReady();
    bool ocrReady = config_.enableOcr && (ocr_ != nullptr || (ocrEngine_ != nullptr && ocrEngine_->IsReady()));
    json::Value obj = json::Value::MakeObject();

    json::Value devices = json::Value::MakeArray();
    if (ready) {
        devices.push_back(json::Value("local"));
    }
    obj.set("devices", devices);
    obj.set("default_device", ready ? json::Value("local") : json::Value());

    bool remote = config_.engine.backend == VlmBackend::kRemote;
    std::string model;
    if (remote) {
        model = config_.engine.model.empty() ? std::string("remote-vlm") : config_.engine.model;
    } else {
        model = Basename(config_.engine.languageModelPath);
        if (model.empty()) {
            model = "on-device-qwen-vl";
        }
    }
    obj.set("model", json::Value(model));
    obj.set("backend", json::Value(remote ? std::string("remote") : std::string("mslite")));
    obj.set("base_url",
            json::Value(remote ? config_.engine.baseUrl : std::string("local://mindspore-lite")));
    obj.set("model_ready", json::Value(ready));
    obj.set("ocr_ready", json::Value(ocrReady));

    json::Value capabilities = json::Value::MakeArray();
    capabilities.push_back(json::Value("analyze"));
    if (config_.enableNodeTree) {
        capabilities.push_back(json::Value("node_tree"));
    }
    if (ocrReady) {
        capabilities.push_back(json::Value("ocr"));
    }
    obj.set("capabilities", capabilities);
    return obj;
}

}  // namespace ScreenParser
}  // namespace OHOS
