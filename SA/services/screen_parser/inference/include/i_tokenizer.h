/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Tokenizer abstraction for the on-device VLM. Concrete implementations
// (byte-level BPE for Qwen2-VL) live beside the inference engine. Keeping this
// as an interface allows unit tests to inject a deterministic fake tokenizer.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_I_TOKENIZER_H
#define FOUNDATION_SCREENPARSER_INFERENCE_I_TOKENIZER_H

#include <cstdint>
#include <string>
#include <vector>

namespace OHOS {
namespace ScreenParser {

class ITokenizer {
public:
    virtual ~ITokenizer() = default;

    // Encode UTF-8 text into token ids.
    virtual std::vector<int32_t> Encode(const std::string &text) const = 0;

    // Decode token ids back into UTF-8 text. Unknown / special ids are skipped.
    virtual std::string Decode(const std::vector<int32_t> &ids) const = 0;

    virtual int32_t VocabSize() const = 0;
    virtual int32_t BosId() const = 0;
    virtual int32_t EosId() const = 0;
    virtual int32_t PadId() const = 0;

    // True when the given id should terminate generation.
    virtual bool IsStopToken(int32_t id) const = 0;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_I_TOKENIZER_H
