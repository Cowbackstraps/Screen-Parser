/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_error.h"

namespace OHOS {
namespace ScreenParser {

const char *ScreenErrorName(ScreenError code) {
    switch (code) {
        case ScreenError::kOk: return "Ok";
        case ScreenError::kNoDevice: return "NoDevice";
        case ScreenError::kDeviceUnavailable: return "DeviceUnavailable";
        case ScreenError::kSensitivePage: return "SensitivePage";
        case ScreenError::kCaptureFailed: return "CaptureFailed";
        case ScreenError::kInvalidRequest: return "InvalidRequest";
        case ScreenError::kModelNotReady: return "ModelNotReady";
        case ScreenError::kEmptyResponse: return "EmptyResponse";
        case ScreenError::kNoJsonObject: return "NoJsonObject";
        case ScreenError::kJsonParseFailed: return "JsonParseFailed";
        case ScreenError::kInferenceFailed: return "InferenceFailed";
        case ScreenError::kTimeout: return "Timeout";
        default: return "Unknown";
    }
}

}  // namespace ScreenParser
}  // namespace OHOS
