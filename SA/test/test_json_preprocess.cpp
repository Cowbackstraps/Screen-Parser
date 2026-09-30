/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for the JSON value model and image preprocessing.

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "image_preprocess.h"
#include "json.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

TEST(JsonTest, ParseAndAccess) {
    Value v = Value::ParseOrThrow("{\"a\": 1, \"b\": \"x\", \"c\": [true, null], \"d\": {}}");
    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(static_cast<int>(v.at("a").as_int()), 1);
    EXPECT_EQ(v.at("b").as_string(), std::string("x"));
    ASSERT_TRUE(v.at("c").is_array());
    EXPECT_EQ(v.at("c").size(), static_cast<size_t>(2));
    EXPECT_TRUE(v.at("c")[static_cast<size_t>(0)].as_bool());
    EXPECT_TRUE(v.at("c")[static_cast<size_t>(1)].is_null());
    EXPECT_TRUE(v.at("d").is_object());
    EXPECT_TRUE(v.at("missing").is_null());
}

TEST(JsonTest, ParseFailure) {
    Value out;
    std::string error;
    EXPECT_FALSE(Value::Parse("{bad json", out, error));
    EXPECT_FALSE(error.empty());
}

TEST(JsonTest, DumpRoundTrip) {
    Value obj = Value::MakeObject();
    obj.set("n", Value(42));
    obj.set("s", Value(std::string("he\"llo")));
    Value arr = Value::MakeArray();
    arr.push_back(Value(1));
    arr.push_back(Value(2.5));
    obj.set("arr", arr);

    std::string text = obj.dump();
    Value reparsed = Value::ParseOrThrow(text);
    EXPECT_EQ(static_cast<int>(reparsed.at("n").as_int()), 42);
    EXPECT_EQ(reparsed.at("s").as_string(), std::string("he\"llo"));
    EXPECT_EQ(reparsed.at("arr").size(), static_cast<size_t>(2));
}

TEST(JsonTest, NumericStringAccessors) {
    Value v = Value::ParseOrThrow("{\"x\": \"12.5\", \"y\": \"abc\"}");
    // Numeric strings are coerced (mirrors Python float()); non-numeric -> fallback.
    EXPECT_NEAR(v.at("x").as_double(0.0), 12.5, 1e-9);
    EXPECT_NEAR(v.at("y").as_double(-1.0), -1.0, 1e-9);
    EXPECT_EQ(v.at("y").as_string_or("def"), std::string("abc"));
}

TEST(PreprocessTest, ResizeProducesExpectedTensorShape) {
    // 4x4 RGB source.
    std::vector<uint8_t> rgb(4 * 4 * 3, 0);
    for (size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 255;      // R
        rgb[i + 1] = 0;    // G
        rgb[i + 2] = 255;  // B
    }
    NormalizeParams params;  // mean 0.5, std 0.5
    ImageTensor tensor = ResizeAndNormalize(rgb.data(), 4, 4, 8, 8, params);
    EXPECT_EQ(tensor.channels, 3);
    EXPECT_EQ(tensor.height, 8);
    EXPECT_EQ(tensor.width, 8);
    EXPECT_EQ(tensor.data.size(), static_cast<size_t>(3 * 8 * 8));

    // R channel: (255/255 - 0.5)/0.5 = 1.0 ; G: (0 - 0.5)/0.5 = -1.0
    EXPECT_NEAR(tensor.data[0], 1.0f, 1e-4);                        // R plane, first pixel
    size_t gOffset = static_cast<size_t>(8 * 8);                    // G plane start
    EXPECT_NEAR(tensor.data[gOffset], -1.0f, 1e-4);
}

TEST(PreprocessTest, BilinearResizeKeepsThreeChannels) {
    std::vector<uint8_t> rgb(2 * 2 * 3, 128);
    std::vector<uint8_t> out = BilinearResizeRgb(rgb.data(), 2, 2, 4, 4);
    EXPECT_EQ(out.size(), static_cast<size_t>(4 * 4 * 3));
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
