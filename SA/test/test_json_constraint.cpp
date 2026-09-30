/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for JSON extraction and the streaming constraint decoder.

#include <string>

#include "gtest/gtest.h"

#include "json.h"
#include "json_constraint_decoder.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

TEST(ExtractJsonTest, PlainObject) {
    Value out;
    std::string error;
    ASSERT_TRUE(ExtractJsonObject("{\"a\": 1}", out, error));
    EXPECT_TRUE(out.is_object());
    EXPECT_EQ(static_cast<int>(out.at("a").as_int()), 1);
}

TEST(ExtractJsonTest, StripsMarkdownFence) {
    Value out;
    std::string error;
    ASSERT_TRUE(ExtractJsonObject("```json\n{\"page\": {\"title\": \"x\"}}\n```", out, error));
    EXPECT_EQ(out.at("page").at("title").as_string(), std::string("x"));
}

TEST(ExtractJsonTest, ExtractsFromSurroundingText) {
    Value out;
    std::string error;
    ASSERT_TRUE(ExtractJsonObject("here you go: {\"ok\": true} hope it helps", out, error));
    EXPECT_TRUE(out.at("ok").as_bool());
}

TEST(ExtractJsonTest, NoObjectFails) {
    Value out;
    std::string error;
    EXPECT_FALSE(ExtractJsonObject("no json here", out, error));
    EXPECT_NE(error.find("no JSON"), std::string::npos);
}

TEST(ExtractJsonTest, MalformedObjectFails) {
    Value out;
    std::string error;
    EXPECT_FALSE(ExtractJsonObject("{\"a\": }", out, error));
    EXPECT_NE(error.find("parse failed"), std::string::npos);
}

TEST(ConstraintDecoderTest, DetectsCompleteObject) {
    JsonConstraintDecoder dec;
    dec.Feed("{\"a\":");
    EXPECT_TRUE(dec.HasStarted());
    EXPECT_FALSE(dec.IsComplete());
    dec.Feed(" [1, 2, 3]}");
    EXPECT_TRUE(dec.IsComplete());
    EXPECT_FALSE(dec.IsBroken());
}

TEST(ConstraintDecoderTest, BracesInsideStringsDoNotClose) {
    JsonConstraintDecoder dec;
    dec.Feed("{\"s\": \"a } b { c\"");
    EXPECT_FALSE(dec.IsComplete());
    dec.Feed("}");
    EXPECT_TRUE(dec.IsComplete());
}

TEST(ConstraintDecoderTest, EscapedQuoteHandled) {
    JsonConstraintDecoder dec;
    dec.Feed("{\"s\": \"quote \\\" brace }\"}");
    EXPECT_TRUE(dec.IsComplete());
}

TEST(ConstraintDecoderTest, UnbalancedIsBroken) {
    JsonConstraintDecoder dec;
    dec.Feed("}}");
    EXPECT_TRUE(dec.IsBroken());
}

TEST(ConstraintDecoderTest, ResetClearsState) {
    JsonConstraintDecoder dec;
    dec.Feed("{\"a\": 1}");
    EXPECT_TRUE(dec.IsComplete());
    dec.Reset();
    EXPECT_FALSE(dec.IsComplete());
    EXPECT_FALSE(dec.HasStarted());
    EXPECT_FALSE(dec.IsBroken());
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
