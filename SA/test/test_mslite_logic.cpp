/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Host-side tests for MSLiteEngine's PURE multimodal helpers.
//
// The on-device MindSpore Lite backend cannot be compiled or run on a dev
// machine (no lite SDK, no model), but the tricky part of local VLM inference —
// embedding text tokens and splicing the vision embeddings into the prompt — is
// factored into always-compiled static functions. These tests pin that logic so
// the image features are genuinely fused (regression guard for the earlier bug
// where EncodeImage output was computed and then discarded).

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "ms_lite_engine.h"

namespace OHOS {
namespace ScreenParser {
namespace {

// A tiny [vocab=3, hidden=2] embedding table:
//   id 0 -> {10, 11}, id 1 -> {20, 21}, id 2 -> {30, 31}
std::vector<float> MakeTable() {
    return {10.0f, 11.0f, 20.0f, 21.0f, 30.0f, 31.0f};
}

constexpr int32_t kHidden = 2;
constexpr int32_t kImageTokenId = 99;  // not in vocab; expands to image embeds.

TEST(MSLiteLogicTest, EmbedTokensLooksUpRowsInOrder) {
    std::vector<float> out = MSLiteEngine::EmbedTokens({0, 2, 1}, MakeTable(), kHidden);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    EXPECT_EQ(out[0], 10.0f);
    EXPECT_EQ(out[1], 11.0f);
    EXPECT_EQ(out[2], 30.0f);
    EXPECT_EQ(out[3], 31.0f);
    EXPECT_EQ(out[4], 20.0f);
    EXPECT_EQ(out[5], 21.0f);
}

TEST(MSLiteLogicTest, EmbedTokensZeroFillsOutOfRange) {
    std::vector<float> out = MSLiteEngine::EmbedTokens({1, 7, -3}, MakeTable(), kHidden);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    EXPECT_EQ(out[0], 20.0f);
    EXPECT_EQ(out[1], 21.0f);
    // id 7 and -3 are out of vocab -> zero rows, layout preserved.
    EXPECT_EQ(out[2], 0.0f);
    EXPECT_EQ(out[3], 0.0f);
    EXPECT_EQ(out[4], 0.0f);
    EXPECT_EQ(out[5], 0.0f);
}

TEST(MSLiteLogicTest, EmbedTokensRejectsBadHidden) {
    EXPECT_TRUE(MSLiteEngine::EmbedTokens({0, 1}, MakeTable(), 0).empty());
    EXPECT_TRUE(MSLiteEngine::EmbedTokens({0, 1}, MakeTable(), -1).empty());
}

// The core fusion guarantee: a single image placeholder token expands into the
// whole vision-embedding block, inline with the surrounding text embeddings.
TEST(MSLiteLogicTest, BuildPromptEmbedsSplicesImageBlock) {
    // Two image tokens: {100,101}, {102,103}.
    std::vector<float> imageEmbeds = {100.0f, 101.0f, 102.0f, 103.0f};
    std::vector<float> out = MSLiteEngine::BuildPromptEmbeds(
        {1, kImageTokenId, 2}, MakeTable(), kHidden, imageEmbeds, kImageTokenId);

    // rows: text(1) + image(2) + text(2) = 4 rows x hidden(2) = 8 floats.
    ASSERT_EQ(out.size(), static_cast<size_t>(8));
    EXPECT_EQ(out[0], 20.0f);   // id 1
    EXPECT_EQ(out[1], 21.0f);
    EXPECT_EQ(out[2], 100.0f);  // image row 0
    EXPECT_EQ(out[3], 101.0f);
    EXPECT_EQ(out[4], 102.0f);  // image row 1
    EXPECT_EQ(out[5], 103.0f);
    EXPECT_EQ(out[6], 30.0f);   // id 2
    EXPECT_EQ(out[7], 31.0f);
}

// With no image supplied, the placeholder must NOT inject garbage: it degrades to
// a zero row (still in-vocab text embeddings are looked up normally).
TEST(MSLiteLogicTest, BuildPromptEmbedsWithoutImageZeroFillsPlaceholder) {
    std::vector<float> out = MSLiteEngine::BuildPromptEmbeds(
        {1, kImageTokenId, 2}, MakeTable(), kHidden, {}, kImageTokenId);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    EXPECT_EQ(out[0], 20.0f);
    EXPECT_EQ(out[1], 21.0f);
    EXPECT_EQ(out[2], 0.0f);  // placeholder -> zero row
    EXPECT_EQ(out[3], 0.0f);
    EXPECT_EQ(out[4], 30.0f);
    EXPECT_EQ(out[5], 31.0f);
}

// A malformed image block (size not a multiple of hidden) is treated as unusable,
// so the placeholder again degrades safely instead of corrupting the layout.
TEST(MSLiteLogicTest, BuildPromptEmbedsRejectsMisalignedImage) {
    std::vector<float> badImage = {1.0f, 2.0f, 3.0f};  // 3 floats, hidden=2
    std::vector<float> out = MSLiteEngine::BuildPromptEmbeds(
        {kImageTokenId}, MakeTable(), kHidden, badImage, kImageTokenId);
    ASSERT_EQ(out.size(), static_cast<size_t>(2));
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[1], 0.0f);
}

// When the image token id is a normal in-vocab token and no image is given, it is
// looked up like any other text token (no special casing leaks).
TEST(MSLiteLogicTest, BuildPromptEmbedsTreatsPlainIdsAsText) {
    std::vector<float> out = MSLiteEngine::BuildPromptEmbeds(
        {0, 1, 2}, MakeTable(), kHidden, {}, kImageTokenId);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    EXPECT_EQ(out[0], 10.0f);
    EXPECT_EQ(out[2], 20.0f);
    EXPECT_EQ(out[4], 30.0f);
}

TEST(MSLiteLogicTest, ArgMaxPicksLargestLogit) {
    const float logits[5] = {0.1f, 0.5f, 9.0f, -1.0f, 2.0f};
    EXPECT_EQ(MSLiteEngine::ArgMax(logits, 5), 2);
    const float neg[3] = {-5.0f, -9.0f, -7.0f};
    EXPECT_EQ(MSLiteEngine::ArgMax(neg, 3), 0);
    EXPECT_EQ(MSLiteEngine::ArgMax(nullptr, 0), 0);
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
