/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// JSON extraction and streaming constraint decoder.
//
//  * ExtractJsonObject() ports analyzer.py::_extract_json: strip Markdown code
//    fences, take the substring from the first '{' to the last '}' and parse it.
//  * JsonConstraintDecoder is a tiny state machine fed with generated text; it
//    reports when a complete top-level JSON object has been emitted (stop
//    condition for autoregressive generation) or when the stream is broken.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_JSON_CONSTRAINT_DECODER_H
#define FOUNDATION_SCREENPARSER_INFERENCE_JSON_CONSTRAINT_DECODER_H

#include <string>

#include "json.h"

namespace OHOS {
namespace ScreenParser {

// Extract and parse the first balanced JSON object embedded in free text.
// Returns false (with error set) when no parseable object is found.
bool ExtractJsonObject(const std::string &content, json::Value &out, std::string &error);

class JsonConstraintDecoder {
public:
    void Reset();

    // Feed newly generated characters. Safe to call incrementally.
    void Feed(const std::string &chunk);

    // A complete top-level object/array has been produced.
    bool IsComplete() const { return complete_; }
    // The stream can no longer form valid JSON.
    bool IsBroken() const { return broken_; }
    // At least one opening brace/bracket has been seen.
    bool HasStarted() const { return started_; }

private:
    size_t consumed_ = 0;
    int depth_ = 0;
    bool inString_ = false;
    bool escape_ = false;
    bool started_ = false;
    bool complete_ = false;
    bool broken_ = false;
    std::string buffer_;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_JSON_CONSTRAINT_DECODER_H
