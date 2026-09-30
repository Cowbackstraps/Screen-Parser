/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "qwen_tokenizer.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "json.h"

namespace OHOS {
namespace ScreenParser {

namespace {

// ---- UTF-8 helpers -------------------------------------------------------

void AppendCodePoint(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Split a UTF-8 string into its constituent code points (as UTF-8 substrings).
std::vector<std::string> SplitCodePoints(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
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
        if (i + step > s.size()) {
            step = s.size() - i;
        }
        out.push_back(s.substr(i, step));
        i += step;
    }
    return out;
}

uint32_t DecodeCodePoint(const std::string &piece) {
    if (piece.empty()) {
        return 0;
    }
    unsigned char ch = static_cast<unsigned char>(piece[0]);
    if (ch < 0x80) {
        return ch;
    }
    if ((ch >> 5) == 0x6 && piece.size() >= 2) {
        return ((ch & 0x1F) << 6) | (static_cast<unsigned char>(piece[1]) & 0x3F);
    }
    if ((ch >> 4) == 0xE && piece.size() >= 3) {
        return ((ch & 0x0F) << 12) | ((static_cast<unsigned char>(piece[1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(piece[2]) & 0x3F);
    }
    if ((ch >> 3) == 0x1E && piece.size() >= 4) {
        return ((ch & 0x07) << 18) | ((static_cast<unsigned char>(piece[1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(piece[2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(piece[3]) & 0x3F);
    }
    return ch;
}

bool IsSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool IsPunctChar(unsigned char c) {
    // ASCII punctuation / symbols (single-byte). Multi-byte characters are
    // treated as "other" so they group with letters.
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

int CharClass(unsigned char c) {
    if (IsSpaceChar(static_cast<char>(c))) {
        return 0;  // space
    }
    if (IsPunctChar(c)) {
        return 1;  // punctuation
    }
    return 2;  // other (letters, digits, multibyte lead bytes)
}

}  // namespace

std::unordered_map<uint8_t, std::string> QwenTokenizer::ByteToUnicode() {
    std::vector<int> bs;
    for (int b = '!'; b <= '~'; ++b) {
        bs.push_back(b);
    }
    for (int b = 0xA1; b <= 0xAC; ++b) {
        bs.push_back(b);
    }
    for (int b = 0xAE; b <= 0xFF; ++b) {
        bs.push_back(b);
    }
    std::vector<int> cs = bs;
    int n = 0;
    for (int b = 0; b < 256; ++b) {
        if (std::find(bs.begin(), bs.end(), b) == bs.end()) {
            bs.push_back(b);
            cs.push_back(256 + n);
            ++n;
        }
    }
    std::unordered_map<uint8_t, std::string> result;
    for (size_t i = 0; i < bs.size(); ++i) {
        std::string piece;
        AppendCodePoint(piece, static_cast<uint32_t>(cs[i]));
        result[static_cast<uint8_t>(bs[i])] = piece;
    }
    return result;
}

std::unordered_map<uint32_t, uint8_t> QwenTokenizer::UnicodeToByte() {
    std::unordered_map<uint32_t, uint8_t> result;
    for (const auto &kv : ByteToUnicode()) {
        result[DecodeCodePoint(kv.second)] = kv.first;
    }
    return result;
}

bool QwenTokenizer::Load(const std::string &vocabPath, const std::string &mergesPath,
                         std::string &error) {
    std::ifstream vocabFile(vocabPath, std::ios::binary);
    if (!vocabFile) {
        error = "cannot open vocab file: " + vocabPath;
        return false;
    }
    std::stringstream vocabBuffer;
    vocabBuffer << vocabFile.rdbuf();

    json::Value vocab;
    if (!json::Value::Parse(vocabBuffer.str(), vocab, error)) {
        error = "invalid vocab json: " + error;
        return false;
    }
    if (!vocab.is_object()) {
        error = "vocab json must be an object";
        return false;
    }

    tokenToId_.clear();
    int32_t maxId = -1;
    for (const auto &kv : vocab.members()) {
        int32_t id = static_cast<int32_t>(kv.second.as_int(-1));
        if (id < 0) {
            continue;
        }
        tokenToId_[kv.first] = id;
        maxId = std::max(maxId, id);
    }
    idToToken_.assign(static_cast<size_t>(maxId) + 1, std::string());
    for (const auto &kv : tokenToId_) {
        idToToken_[static_cast<size_t>(kv.second)] = kv.first;
    }
    vocabSize_ = static_cast<int32_t>(idToToken_.size());

    mergeRanks_.clear();
    std::ifstream mergesFile(mergesPath, std::ios::binary);
    if (!mergesFile) {
        error = "cannot open merges file: " + mergesPath;
        return false;
    }
    std::string line;
    int32_t rank = 0;
    while (std::getline(mergesFile, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;  // skip header / blank lines
        }
        mergeRanks_[line] = rank++;
    }
    return true;
}

void QwenTokenizer::SetSpecialTokens(int32_t bos, int32_t eos, int32_t pad) {
    bosId_ = bos;
    eosId_ = eos;
    padId_ = pad;
}

bool QwenTokenizer::IsStopToken(int32_t id) const {
    return eosId_ >= 0 && id == eosId_;
}

std::vector<std::string> QwenTokenizer::PreTokenize(const std::string &text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int cls = CharClass(lead);
        size_t start = i;
        if (cls == 2 && lead >= 0x80) {
            // Consume a whole multi-byte sequence as one unit.
            size_t step = 1;
            if ((lead >> 5) == 0x6) {
                step = 2;
            } else if ((lead >> 4) == 0xE) {
                step = 3;
            } else if ((lead >> 3) == 0x1E) {
                step = 4;
            }
            i = std::min(text.size(), i + step);
            tokens.push_back(text.substr(start, i - start));
            continue;
        }
        // Group consecutive ASCII characters of the same class.
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0x80 || CharClass(c) != cls) {
                break;
            }
            ++i;
        }
        if (i == start) {
            ++i;  // guarantee progress
        }
        tokens.push_back(text.substr(start, i - start));
    }
    return tokens;
}

std::string QwenTokenizer::BytesToSymbols(const std::string &utf8) {
    static const std::unordered_map<uint8_t, std::string> b2u = ByteToUnicode();
    std::string out;
    for (unsigned char byte : utf8) {
        auto it = b2u.find(byte);
        if (it != b2u.end()) {
            out += it->second;
        }
    }
    return out;
}

std::vector<std::string> QwenTokenizer::ApplyBpe(const std::vector<std::string> &symbols) const {
    std::vector<std::string> word = symbols;
    if (word.size() < 2) {
        return word;
    }
    while (true) {
        // Find the lowest-rank adjacent pair present in merges.
        int bestRank = -1;
        size_t bestIndex = 0;
        for (size_t i = 0; i + 1 < word.size(); ++i) {
            std::string pair = word[i] + " " + word[i + 1];
            auto it = mergeRanks_.find(pair);
            if (it != mergeRanks_.end() && (bestRank < 0 || it->second < bestRank)) {
                bestRank = it->second;
                bestIndex = i;
            }
        }
        if (bestRank < 0) {
            break;
        }
        // Merge every occurrence of the chosen pair.
        std::string first = word[bestIndex];
        std::string second = word[bestIndex + 1];
        std::vector<std::string> merged;
        for (size_t i = 0; i < word.size();) {
            if (i + 1 < word.size() && word[i] == first && word[i + 1] == second) {
                merged.push_back(first + second);
                i += 2;
            } else {
                merged.push_back(word[i]);
                ++i;
            }
        }
        word = std::move(merged);
        if (word.size() < 2) {
            break;
        }
    }
    return word;
}

std::vector<int32_t> QwenTokenizer::Encode(const std::string &text) const {
    std::vector<int32_t> ids;
    for (const auto &pretoken : PreTokenize(text)) {
        std::string symbolSpace = BytesToSymbols(pretoken);
        std::vector<std::string> symbols = SplitCodePoints(symbolSpace);
        std::vector<std::string> tokens = ApplyBpe(symbols);
        for (const auto &token : tokens) {
            auto it = tokenToId_.find(token);
            if (it != tokenToId_.end()) {
                ids.push_back(it->second);
            }
            // Unknown tokens are dropped; byte-level vocab should cover all.
        }
    }
    return ids;
}

std::string QwenTokenizer::Decode(const std::vector<int32_t> &ids) const {
    static const std::unordered_map<uint32_t, uint8_t> u2b = UnicodeToByte();
    std::string symbolSpace;
    for (int32_t id : ids) {
        if (id < 0 || id >= static_cast<int32_t>(idToToken_.size())) {
            continue;
        }
        if (IsStopToken(id)) {
            continue;
        }
        symbolSpace += idToToken_[static_cast<size_t>(id)];
    }
    // Map the unicode-alphabet symbols back to raw bytes.
    std::string bytes;
    for (const auto &piece : SplitCodePoints(symbolSpace)) {
        auto it = u2b.find(DecodeCodePoint(piece));
        if (it != u2b.end()) {
            bytes.push_back(static_cast<char>(it->second));
        }
    }
    return bytes;
}

}  // namespace ScreenParser
}  // namespace OHOS
