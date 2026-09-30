/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "json_constraint_decoder.h"

#include <cctype>

namespace OHOS {
namespace ScreenParser {

namespace {

std::string Trim(const std::string &s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Remove leading ```json / ``` fence and trailing ``` fence.
std::string StripCodeFence(const std::string &s) {
    std::string out = s;
    if (out.rfind("```", 0) == 0) {
        size_t nl = out.find('\n');
        out = (nl == std::string::npos) ? std::string() : out.substr(nl + 1);
    }
    size_t fence = out.rfind("```");
    if (fence != std::string::npos) {
        out = out.substr(0, fence);
    }
    return out;
}

}  // namespace

bool ExtractJsonObject(const std::string &content, json::Value &out, std::string &error) {
    std::string cleaned = StripCodeFence(Trim(content));
    size_t start = cleaned.find('{');
    size_t end = cleaned.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        error = "no JSON object in model response";
        return false;
    }
    std::string slice = cleaned.substr(start, end - start + 1);
    if (!json::Value::Parse(slice, out, error)) {
        error = "JSON parse failed: " + error;
        return false;
    }
    if (!out.is_object()) {
        error = "model response must be a JSON object";
        return false;
    }
    return true;
}

void JsonConstraintDecoder::Reset() {
    consumed_ = 0;
    depth_ = 0;
    inString_ = false;
    escape_ = false;
    started_ = false;
    complete_ = false;
    broken_ = false;
    buffer_.clear();
}

void JsonConstraintDecoder::Feed(const std::string &chunk) {
    buffer_ += chunk;
    for (; consumed_ < buffer_.size(); ++consumed_) {
        char ch = buffer_[consumed_];
        if (inString_) {
            if (escape_) {
                escape_ = false;
            } else if (ch == '\\') {
                escape_ = true;
            } else if (ch == '"') {
                inString_ = false;
            }
            continue;
        }
        switch (ch) {
            case '"':
                inString_ = true;
                break;
            case '{':
            case '[':
                ++depth_;
                started_ = true;
                break;
            case '}':
            case ']':
                --depth_;
                if (depth_ < 0) {
                    broken_ = true;
                } else if (depth_ == 0 && started_) {
                    complete_ = true;
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace ScreenParser
}  // namespace OHOS
