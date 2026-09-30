/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_schema.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace OHOS {
namespace ScreenParser {

namespace {

// Convert a JSON value to a coordinate double. Accepts numbers and numeric
// strings (like Python's float()); rejects null/bool/array/object.
bool ToCoordinate(const json::Value &value, double &out) {
    if (value.is_number()) {
        out = value.as_double();
        return std::isfinite(out);
    }
    if (value.is_string()) {
        const std::string &s = value.as_string();
        if (s.empty()) {
            return false;
        }
        char *end = nullptr;
        double parsed = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') {
            return false;
        }
        out = parsed;
        return std::isfinite(out);
    }
    return false;
}

int32_t ClampCoord(double value) {
    long long rounded = std::llround(value);
    if (rounded < 0) {
        rounded = 0;
    }
    if (rounded > 1000) {
        rounded = 1000;
    }
    return static_cast<int32_t>(rounded);
}

}  // namespace

bool Bounds::FromValue(const json::Value &value, int32_t screenWidth, int32_t screenHeight,
                       Bounds &out) {
    double coords[4] = {0.0, 0.0, 0.0, 0.0};

    if (value.is_object()) {
        const char *keys[4] = {"x1", "y1", "x2", "y2"};
        for (int i = 0; i < 4; ++i) {
            if (!ToCoordinate(value.at(keys[i]), coords[i])) {
                return false;
            }
        }
    } else if (value.is_array() && value.size() == 4) {
        const json::Array &arr = value.items();
        for (int i = 0; i < 4; ++i) {
            if (!ToCoordinate(arr[static_cast<size_t>(i)], coords[i])) {
                return false;
            }
        }
    } else {
        return false;
    }

    // Accept pixel coordinates as a defensive fallback.
    if (coords[2] > 1000.0 || coords[3] > 1000.0) {
        if (screenWidth <= 0 || screenHeight <= 0) {
            return false;
        }
        coords[0] = coords[0] / screenWidth * 1000.0;
        coords[1] = coords[1] / screenHeight * 1000.0;
        coords[2] = coords[2] / screenWidth * 1000.0;
        coords[3] = coords[3] / screenHeight * 1000.0;
    }

    int32_t x1 = ClampCoord(coords[0]);
    int32_t y1 = ClampCoord(coords[1]);
    int32_t x2 = ClampCoord(coords[2]);
    int32_t y2 = ClampCoord(coords[3]);
    if (x1 > x2) {
        std::swap(x1, x2);
    }
    if (y1 > y2) {
        std::swap(y1, y2);
    }
    if (x2 - x1 < 2 || y2 - y1 < 2) {
        return false;
    }

    out.x1 = x1;
    out.y1 = y1;
    out.x2 = x2;
    out.y2 = y2;
    return true;
}

json::Value Bounds::ToJson() const {
    json::Value obj = json::Value::MakeObject();
    obj.set("x1", json::Value(x1));
    obj.set("y1", json::Value(y1));
    obj.set("x2", json::Value(x2));
    obj.set("y2", json::Value(y2));
    return obj;
}

json::Value ScreenNode::ToJson() const {
    json::Value obj = json::Value::MakeObject();
    obj.set("id", json::Value(id));
    obj.set("label", json::Value(label));
    obj.set("role", json::Value(role));
    obj.set("bounds", bounds.ToJson());
    obj.set("confidence", json::Value(confidence));
    obj.set("interactive", json::Value(interactive));
    obj.set("evidence", json::Value(evidence));
    obj.set("description", json::Value(description));
    obj.set("source", json::Value(source));
    obj.set("text", json::Value(text));
    // Hierarchy fields.
    obj.set("parent_id", json::Value(parentId));
    obj.set("depth", json::Value(depth));
    json::Value childIds = json::Value::MakeArray();
    for (const auto &child : children) {
        childIds.push_back(json::Value(child));
    }
    obj.set("children", childIds);
    return obj;
}

json::Value PageSummary::ToJson() const {
    json::Value obj = json::Value::MakeObject();
    obj.set("title", json::Value(title));
    obj.set("page_type", json::Value(pageType));
    obj.set("summary", json::Value(summary));
    return obj;
}

json::Value ScreenAnalysis::ToJson() const {
    json::Value obj = json::Value::MakeObject();
    obj.set("page", page.ToJson());

    json::Value nodeArray = json::Value::MakeArray();
    for (const auto &node : nodes) {
        nodeArray.push_back(node.ToJson());
    }
    obj.set("nodes", nodeArray);
    obj.set("tree", BuildNodeTreeJson(nodes));

    obj.set("current_app", json::Value(currentApp));
    obj.set("screen_width", json::Value(screenWidth));
    obj.set("screen_height", json::Value(screenHeight));
    obj.set("ocr_text", json::Value(ocrText));
    return obj;
}

double BoundsArea(const Bounds &bounds) {
    double w = static_cast<double>(bounds.x2 - bounds.x1);
    double h = static_cast<double>(bounds.y2 - bounds.y1);
    if (w <= 0.0 || h <= 0.0) {
        return 0.0;
    }
    return w * h;
}

double BoundsContainment(const Bounds &inner, const Bounds &outer) {
    double innerArea = BoundsArea(inner);
    if (innerArea <= 0.0) {
        return 0.0;
    }
    int32_t ix1 = std::max(inner.x1, outer.x1);
    int32_t iy1 = std::max(inner.y1, outer.y1);
    int32_t ix2 = std::min(inner.x2, outer.x2);
    int32_t iy2 = std::min(inner.y2, outer.y2);
    int32_t iw = ix2 - ix1;
    int32_t ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) {
        return 0.0;
    }
    return (static_cast<double>(iw) * ih) / innerArea;
}

namespace {

// A node is considered a child of another when the container covers at least
// this fraction of its area.
constexpr double kContainThreshold = 0.85;
constexpr size_t kNoParent = static_cast<size_t>(-1);

json::Value NodeToJsonRecursive(const ScreenNode &node,
                                const std::unordered_map<std::string, const ScreenNode *> &byId,
                                int guard) {
    json::Value obj = node.ToJson();
    json::Value childArray = json::Value::MakeArray();
    if (guard > 0) {
        for (const auto &childId : node.children) {
            auto it = byId.find(childId);
            if (it != byId.end()) {
                childArray.push_back(NodeToJsonRecursive(*it->second, byId, guard - 1));
            }
        }
    }
    obj.set("children", childArray);
    return obj;
}

}  // namespace

void BuildNodeTree(std::vector<ScreenNode> &nodes) {
    const size_t n = nodes.size();
    for (auto &node : nodes) {
        node.children.clear();
        node.depth = 0;
    }
    if (n == 0) {
        return;
    }

    std::unordered_map<std::string, size_t> idToIndex;
    idToIndex.reserve(n * 2);
    std::vector<double> areas(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        idToIndex[nodes[i].id] = i;
        areas[i] = BoundsArea(nodes[i].bounds);
    }

    std::vector<size_t> parentIdx(n, kNoParent);
    for (size_t i = 0; i < n; ++i) {
        // Honor an explicit parent id when it references a different valid node.
        if (!nodes[i].parentId.empty()) {
            auto it = idToIndex.find(nodes[i].parentId);
            if (it != idToIndex.end() && it->second != i) {
                parentIdx[i] = it->second;
                continue;
            }
            nodes[i].parentId.clear();  // invalid -> fall back to geometry
        }
        // Geometric inference: smallest strictly-larger container.
        size_t best = kNoParent;
        double bestArea = 0.0;
        for (size_t j = 0; j < n; ++j) {
            if (j == i || areas[j] <= areas[i]) {
                continue;
            }
            if (BoundsContainment(nodes[i].bounds, nodes[j].bounds) < kContainThreshold) {
                continue;
            }
            if (best == kNoParent || areas[j] < bestArea) {
                best = j;
                bestArea = areas[j];
            }
        }
        parentIdx[i] = best;
    }

    // Detach any node whose ancestor chain loops back to itself (can only happen
    // with model-supplied parent ids).
    for (size_t i = 0; i < n; ++i) {
        size_t cur = i;
        for (size_t step = 0; step <= n; ++step) {
            size_t p = parentIdx[cur];
            if (p == kNoParent) {
                break;
            }
            if (p == i) {
                parentIdx[i] = kNoParent;
                break;
            }
            cur = p;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (parentIdx[i] == kNoParent) {
            nodes[i].parentId.clear();
        } else {
            nodes[i].parentId = nodes[parentIdx[i]].id;
            nodes[parentIdx[i]].children.push_back(nodes[i].id);
        }
    }

    // Sort each child list in reading order (top-to-bottom, then left-to-right).
    for (size_t i = 0; i < n; ++i) {
        std::vector<std::string> &children = nodes[i].children;
        std::stable_sort(children.begin(), children.end(),
                         [&](const std::string &a, const std::string &b) {
                             const Bounds &ba = nodes[idToIndex[a]].bounds;
                             const Bounds &bb = nodes[idToIndex[b]].bounds;
                             if (ba.y1 != bb.y1) {
                                 return ba.y1 < bb.y1;
                             }
                             return ba.x1 < bb.x1;
                         });
    }

    // Compute depths by walking each node's chain up to a known/root ancestor.
    std::vector<int32_t> depth(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] >= 0) {
            continue;
        }
        std::vector<size_t> chain;
        size_t cur = i;
        while (true) {
            chain.push_back(cur);
            size_t p = parentIdx[cur];
            if (p == kNoParent || depth[p] >= 0) {
                break;
            }
            if (std::find(chain.begin(), chain.end(), p) != chain.end()) {
                break;  // safety against residual cycles
            }
            cur = p;
        }
        size_t top = chain.back();
        int32_t base = 0;
        if (parentIdx[top] != kNoParent && depth[parentIdx[top]] >= 0) {
            base = depth[parentIdx[top]] + 1;
        }
        int32_t d = base;
        for (size_t k = chain.size(); k-- > 0;) {
            depth[chain[k]] = d;
            ++d;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        nodes[i].depth = depth[i] < 0 ? 0 : depth[i];
    }
}

json::Value BuildNodeTreeJson(const std::vector<ScreenNode> &nodes) {
    std::unordered_map<std::string, const ScreenNode *> byId;
    byId.reserve(nodes.size() * 2);
    for (const auto &node : nodes) {
        byId[node.id] = &node;
    }
    json::Value roots = json::Value::MakeArray();
    int guard = static_cast<int>(nodes.size()) + 1;
    for (const auto &node : nodes) {
        if (node.parentId.empty()) {
            roots.push_back(NodeToJsonRecursive(node, byId, guard));
        }
    }
    return roots;
}

std::string TruncateUtf8(const std::string &text, size_t maxChars) {
    size_t chars = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        if (ch < 0x80) {
            step = 1;
        } else if ((ch >> 5) == 0x6) {
            step = 2;
        } else if ((ch >> 4) == 0xE) {
            step = 3;
        } else if ((ch >> 3) == 0x1E) {
            step = 4;
        }
        if (chars + 1 > maxChars) {
            return text.substr(0, i);
        }
        if (i + step > text.size()) {
            // Malformed trailing sequence: stop here.
            return text.substr(0, i);
        }
        i += step;
        ++chars;
    }
    return text;
}

}  // namespace ScreenParser
}  // namespace OHOS
