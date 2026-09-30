/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_capture.h"

#include "base64.h"

#if defined(SCREENPARSER_ENABLE_CAPTURE)
#include "image_packer.h"
#include "pixelmap.h"
#include "pixelmap_native.h"
#include "screenshot.h"
#include "display_manager.h"
#endif

namespace OHOS {
namespace ScreenParser {

std::vector<uint8_t> ScreenCapture::RgbaToRgb(const uint8_t *rgba, size_t pixelCount) {
    std::vector<uint8_t> rgb;
    if (rgba == nullptr || pixelCount == 0) {
        return rgb;
    }
    rgb.resize(pixelCount * 3);
    for (size_t i = 0; i < pixelCount; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return rgb;
}

#if defined(SCREENPARSER_ENABLE_CAPTURE)

CapturedScreen ScreenCapture::Capture() {
    CapturedScreen result;

    // Resolve the default display.
    auto display = OHOS::DisplayManager::GetInstance().GetDefaultDisplay();
    if (display == nullptr) {
        throw ScreenInputError(ScreenError::kCaptureFailed, "无法获取默认显示屏");
    }
    int32_t displayId = display->GetId();
    uint32_t screenW = display->GetWidth();
    uint32_t screenH = display->GetHeight();

    // Capture the whole screen. Returns nullptr when the current window has the
    // FLAG_SECURE / privacy attribute set (sensitive page).
    std::shared_ptr<Media::PixelMap> pixelMap =
        OHOS::Screenshot::CaptureScreen(displayId, 0.0f, screenW, screenH);
    if (pixelMap == nullptr) {
        throw ScreenInputError(ScreenError::kSensitivePage, "当前页面禁止截图，无法进行视觉解析");
    }

    Media::ImageInfo info = pixelMap->GetImageInfo();
    int32_t width = static_cast<int32_t>(info.size.width);
    int32_t height = static_cast<int32_t>(info.size.height);
    size_t byteCount = pixelMap->GetByteCount();
    std::vector<uint8_t> buffer(byteCount);
    if (pixelMap->ReadPixels(reinterpret_cast<char *>(buffer.data()), byteCount) !=
        Media::ERR_OK) {
        throw ScreenInputError(ScreenError::kCaptureFailed, "读取屏幕像素失败");
    }

    result.width = width;
    result.height = height;
    result.rgba = std::move(buffer);  // PixelMap format is RGBA_8888
    result.isSensitive = false;
    return result;
}

#else

CapturedScreen ScreenCapture::Capture() {
    throw ScreenInputError(ScreenError::kCaptureFailed,
                           "截屏能力未在此构建中启用（SCREENPARSER_ENABLE_CAPTURE）");
}

#endif

#if defined(SCREENPARSER_ENABLE_CAPTURE)

std::string ScreenCapture::EncodePngDataUri(const CapturedScreen &screen) {
    if (screen.rgba.empty() || screen.width <= 0 || screen.height <= 0) {
        return std::string();
    }
    // Build a PixelMap from the captured RGBA buffer, then pack it as PNG.
    Media::InitializationOptions options;
    options.size.width = screen.width;
    options.size.height = screen.height;
    options.pixelFormat = Media::PixelFormat::RGBA_8888;
    options.editable = false;
    std::shared_ptr<Media::PixelMap> pixelMap = Media::PixelMap::Create(options, nullptr);
    if (pixelMap == nullptr) {
        return std::string();
    }
    pixelMap->WritePixels(reinterpret_cast<const char *>(screen.rgba.data()), screen.rgba.size());

    std::unique_ptr<Media::ImagePacker> packer(Media::ImagePackerFactory::GetInstance());
    if (packer == nullptr) {
        return std::string();
    }
    Media::PackingOption packOption;
    packOption.format = "image/png";
    packOption.quality = 100;
    uint8_t *outData = nullptr;
    size_t outSize = 0;
    if (packer->PackToData(*pixelMap, packOption, &outData, outSize) != Media::ERR_OK ||
        outData == nullptr) {
        return std::string();
    }
    std::string encoded = Base64Encode(outData, outSize);
    Media::ImagePacker::FreeData(outData);
    return "data:image/png;base64," + encoded;
}

#else

std::string ScreenCapture::EncodePngDataUri(const CapturedScreen &) {
    return std::string();
}

#endif

}  // namespace ScreenParser
}  // namespace OHOS
