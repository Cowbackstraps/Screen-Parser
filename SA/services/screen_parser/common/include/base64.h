/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Minimal base64 encoder used to embed the screenshot preview into the result
// JSON (mirrors the "data:image/png;base64,..." payload of the prototype).

#ifndef FOUNDATION_SCREENPARSER_COMMON_BASE64_H
#define FOUNDATION_SCREENPARSER_COMMON_BASE64_H

#include <cstdint>
#include <string>
#include <vector>

namespace OHOS {
namespace ScreenParser {

std::string Base64Encode(const uint8_t *data, size_t length);
inline std::string Base64Encode(const std::vector<uint8_t> &data) {
    return Base64Encode(data.data(), data.size());
}

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_COMMON_BASE64_H
