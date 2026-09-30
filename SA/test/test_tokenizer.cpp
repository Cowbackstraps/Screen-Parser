/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for the byte-level BPE tokenizer. A minimal single-byte vocab is
// generated on the fly (from ByteToUnicode) so encode/decode round-trips can be
// verified without shipping the full Qwen2-VL vocab asset.

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"

#include "json.h"
#include "qwen_tokenizer.h"

namespace OHOS {
namespace ScreenParser {
namespace {

TEST(ByteAlphabetTest, CoversAllBytesAndRoundTrips) {
    auto b2u = QwenTokenizer::ByteToUnicode();
    EXPECT_EQ(b2u.size(), static_cast<size_t>(256));

    auto u2b = QwenTokenizer::UnicodeToByte();
    EXPECT_EQ(u2b.size(), static_cast<size_t>(256));

    // Every byte maps to a unique unicode symbol and back.
    for (int byte = 0; byte < 256; ++byte) {
        auto it = b2u.find(static_cast<uint8_t>(byte));
        ASSERT_TRUE(it != b2u.end());
        // Decode the symbol's code point and map back to the original byte.
        const std::string &symbol = it->second;
        uint32_t cp = 0;
        // symbols are single code points; recover it via UTF-8 lead byte logic
        unsigned char lead = static_cast<unsigned char>(symbol[0]);
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead >> 5) == 0x6) {
            cp = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(symbol[1]) & 0x3F);
        } else if ((lead >> 4) == 0xE) {
            cp = ((lead & 0x0F) << 12) |
                 ((static_cast<unsigned char>(symbol[1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(symbol[2]) & 0x3F);
        } else {
            cp = lead;
        }
        auto back = u2b.find(cp);
        ASSERT_TRUE(back != u2b.end());
        EXPECT_EQ(static_cast<int>(back->second), byte);
    }
}

// Build a vocab file mapping each single byte-symbol to a unique id, plus an
// empty merges file. With no merges, every byte becomes its own token.
bool WriteMinimalVocab(const std::string &vocabPath, const std::string &mergesPath) {
    auto b2u = QwenTokenizer::ByteToUnicode();
    json::Value vocab = json::Value::MakeObject();
    for (int byte = 0; byte < 256; ++byte) {
        const std::string &symbol = b2u[static_cast<uint8_t>(byte)];
        vocab.set(symbol, json::Value(byte));
    }
    std::ofstream vf(vocabPath, std::ios::binary);
    if (!vf) {
        return false;
    }
    vf << vocab.dump();
    vf.close();

    std::ofstream mf(mergesPath, std::ios::binary);
    if (!mf) {
        return false;
    }
    mf << "#version: 0.2\n";
    mf.close();
    return true;
}

TEST(TokenizerTest, EncodeDecodeRoundTrip) {
    const std::string vocabPath = "test_vocab.json";
    const std::string mergesPath = "test_merges.txt";
    ASSERT_TRUE(WriteMinimalVocab(vocabPath, mergesPath));

    QwenTokenizer tok;
    std::string error;
    ASSERT_TRUE(tok.Load(vocabPath, mergesPath, error));
    EXPECT_EQ(tok.VocabSize(), 256);

    const std::string text = "hello world {\"a\": 1}";
    std::vector<int32_t> ids = tok.Encode(text);
    EXPECT_FALSE(ids.empty());
    EXPECT_EQ(tok.Decode(ids), text);

    std::remove(vocabPath.c_str());
    std::remove(mergesPath.c_str());
}

TEST(TokenizerTest, LoadFailsOnMissingFile) {
    QwenTokenizer tok;
    std::string error;
    EXPECT_FALSE(tok.Load("does_not_exist_vocab.json", "does_not_exist_merges.txt", error));
    EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
