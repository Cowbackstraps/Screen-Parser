/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Screen capture for the on-device service.
//
// On OpenHarmony this uses the system screenshot capability (foundation/window
// Screenshot interface), which returns a PixelMap. The raw RGBA pixels are
// copied into CapturedScreen so the rest of the pipeline stays device-agnostic
// and unit-testable. Requires ohos.permission.CAPTURE_SCREEN (system_basic APL).
//
// When SCREENPARSER_ENABLE_CAPTURE is not defined (host unit-test builds), the
// capture entry point reports kCaptureFailed instead of linking window headers.

#ifndef FOUNDATION_SCREENPARSER_CAPTURE_SCREEN_CAPTURE_H
#define FOUNDATION_SCREENPARSER_CAPTURE_SCREEN_CAPTURE_H

#include <cstdint>
#include <string>
#include <vector>

#include "screen_error.h"

namespace OHOS {
namespace ScreenParser {

struct CapturedScreen {
    std::vector<uint8_t> rgba;  // width * height * 4
    int32_t width = 0;
    int32_t height = 0;
    bool isSensitive = false;  // page disallows screenshot
};

class ScreenCapture {
public:
    // Capture the current screen. Throws ScreenInputError on failure.
    static CapturedScreen Capture();

    // Pure helper: convert an RGBA buffer to packed RGB (drops alpha).
    static std::vector<uint8_t> RgbaToRgb(const uint8_t *rgba, size_t pixelCount);

    // Encode the captured screen as a "data:image/png;base64,..." URI for the
    // web preview. Uses the OHOS image packer on device; returns an empty
    // string when the capture/encode backend is not compiled in.
    static std::string EncodePngDataUri(const CapturedScreen &screen);
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_CAPTURE_SCREEN_CAPTURE_H
