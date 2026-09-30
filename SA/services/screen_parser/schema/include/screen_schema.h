/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Structured output models for screen understanding.
// Direct port of screen/schemas.py: Bounds (0-1000 normalized box),
// ScreenNode, PageSummary and ScreenAnalysis.

#ifndef FOUNDATION_SCREENPARSER_SCHEMA_SCREEN_SCHEMA_H
#define FOUNDATION_SCREENPARSER_SCHEMA_SCREEN_SCHEMA_H

#include <cstdint>
#include <string>
#include <vector>

#include "json.h"

namespace OHOS {
namespace ScreenParser {

// Normalized bounding box in the 0-1000 coordinate space, origin top-left.
struct Bounds {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;

    // Build a Bounds from a model-produced value (array of 4 numbers or an
    // object with x1/y1/x2/y2 keys). Accepts pixel coordinates as a defensive
    // fallback when values exceed 1000. Returns false when the value cannot be
    // interpreted or the box is degenerate (side < 2), matching the prototype.
    static bool FromValue(const json::Value &value, int32_t screenWidth, int32_t screenHeight,
                          Bounds &out);

    json::Value ToJson() const;
};

struct ScreenNode {
    std::string id;
    std::string label;
    std::string role;
    Bounds bounds;
    double confidence = 0.0;
    bool interactive = false;
    std::string evidence = "vision";  // "vision" | "pending"
    std::string description;
    // Provenance of the node: "vlm" (from the vision-language model),
    // "ocr" (synthesized from an OCR text block) or "fused".
    std::string source = "vlm";
    // Raw recognized text when this node is backed by OCR.
    std::string text;

    // Hierarchy fields, populated by BuildNodeTree(). parentId is empty for
    // top-level (root) nodes; children holds ids in reading order.
    std::string parentId;
    int32_t depth = 0;
    std::vector<std::string> children;

    json::Value ToJson() const;
};

// Geometric helpers used for hierarchy inference (0-1000 space).
double BoundsArea(const Bounds &bounds);
// Fraction of `inner` area covered by `outer` (0..1); 0 when they do not overlap.
double BoundsContainment(const Bounds &inner, const Bounds &outer);

struct PageSummary {
    std::string title;
    std::string pageType = "unknown";
    std::string summary;

    json::Value ToJson() const;
};

struct ScreenAnalysis {
    PageSummary page;
    std::vector<ScreenNode> nodes;
    std::string currentApp = "unknown";
    int32_t screenWidth = 0;
    int32_t screenHeight = 0;
    // Full-page OCR text (all recognized blocks joined), empty when OCR is off.
    std::string ocrText;

    json::Value ToJson() const;
};

// Infer a node hierarchy from geometry: a node whose box (almost) fully
// contains another, larger-area node becomes its parent; the smallest such
// container is chosen as the direct parent. Fills parentId / depth / children
// for every node in place. Deterministic and cycle-free (parents are strictly
// larger). Optionally an explicit "parent_id" produced by the model is honored
// when it references an existing node id.
void BuildNodeTree(std::vector<ScreenNode> &nodes);

// Build the nested tree JSON (roots with recursive children) from a flat,
// already-linked node list. Used by ScreenAnalysis::ToJson().
json::Value BuildNodeTreeJson(const std::vector<ScreenNode> &nodes);

// Clamp a double into [lo, hi].
inline double ClampDouble(double value, double lo, double hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// Truncate a UTF-8 string to at most maxChars *characters* (not bytes),
// never splitting a multi-byte sequence. Mirrors Python's str[:n] slicing.
std::string TruncateUtf8(const std::string &text, size_t maxChars);

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_SCHEMA_SCREEN_SCHEMA_H
