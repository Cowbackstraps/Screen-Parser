/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Byte-level BPE tokenizer compatible with the Qwen2-VL family.
//
// Loads two assets exported alongside the converted model:
//   * vocab.json  : { "<token string>": <id>, ... }
//   * merges.txt  : BPE merge rules, one "<a> <b>" pair per line, ordered.
//
// The implementation follows the standard GPT-2 byte-level BPE scheme: raw
// UTF-8 bytes are mapped to a printable unicode alphabet, pre-tokenized, then
// merged according to the learned ranks. A simplified pre-tokenization regex
// (whitespace / punctuation boundaries) is used, which is sufficient for the
// constrained JSON output this service generates.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_QWEN_TOKENIZER_H
#define FOUNDATION_SCREENPARSER_INFERENCE_QWEN_TOKENIZER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "i_tokenizer.h"

namespace OHOS {
namespace ScreenParser {

class QwenTokenizer : public ITokenizer {
public:
    QwenTokenizer() = default;

    // Load vocab.json and merges.txt. Returns false (and sets error) on failure.
    bool Load(const std::string &vocabPath, const std::string &mergesPath, std::string &error);

    // Configure special token ids (defaults follow Qwen2 conventions).
    void SetSpecialTokens(int32_t bos, int32_t eos, int32_t pad);

    std::vector<int32_t> Encode(const std::string &text) const override;
    std::string Decode(const std::vector<int32_t> &ids) const override;

    int32_t VocabSize() const override { return vocabSize_; }
    int32_t BosId() const override { return bosId_; }
    int32_t EosId() const override { return eosId_; }
    int32_t PadId() const override { return padId_; }
    bool IsStopToken(int32_t id) const override;

    // Exposed for tests: byte <-> unicode alphabet mapping.
    static std::unordered_map<uint8_t, std::string> ByteToUnicode();
    static std::unordered_map<uint32_t, uint8_t> UnicodeToByte();

private:
    // Split text into pre-tokens (substrings) using boundary rules.
    static std::vector<std::string> PreTokenize(const std::string &text);
    // Map a UTF-8 substring to the unicode-alphabet symbol string.
    static std::string BytesToSymbols(const std::string &utf8);
    // Apply BPE merges to a symbol sequence represented as code points.
    std::vector<std::string> ApplyBpe(const std::vector<std::string> &symbols) const;

    std::unordered_map<std::string, int32_t> tokenToId_;
    std::vector<std::string> idToToken_;
    std::unordered_map<std::string, int32_t> mergeRanks_;  // "a b" -> rank
    int32_t vocabSize_ = 0;
    int32_t bosId_ = -1;
    int32_t eosId_ = -1;
    int32_t padId_ = -1;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_QWEN_TOKENIZER_H
