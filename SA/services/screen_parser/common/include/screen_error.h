/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Error codes and exceptions for the screen parser service. These mirror the
// two exception types of the Python prototype:
//   ScreenInputError -> kInput*  (device / request cannot be analyzed, HTTP 400)
//   ScreenModelError -> kModel*  (model output cannot be parsed, HTTP 502)

#ifndef FOUNDATION_SCREENPARSER_COMMON_SCREEN_ERROR_H
#define FOUNDATION_SCREENPARSER_COMMON_SCREEN_ERROR_H

#include <stdexcept>
#include <string>

namespace OHOS {
namespace ScreenParser {

enum class ScreenError {
    kOk = 0,
    // Input / device side.
    kNoDevice = 100,
    kDeviceUnavailable = 101,
    kSensitivePage = 102,
    kCaptureFailed = 103,
    kInvalidRequest = 104,
    // Model / inference side.
    kModelNotReady = 200,
    kEmptyResponse = 201,
    kNoJsonObject = 202,
    kJsonParseFailed = 203,
    kInferenceFailed = 204,
    kTimeout = 205,
};

// Base exception carrying a ScreenError code and a human-readable message.
class ScreenException : public std::runtime_error {
public:
    ScreenException(ScreenError code, const std::string &message)
        : std::runtime_error(message), code_(code) {}
    ScreenError code() const { return code_; }

    // True when the failure is caused by the local device / request
    // (maps to HTTP 400 in the debug server).
    bool is_input_error() const {
        return code_ == ScreenError::kNoDevice || code_ == ScreenError::kDeviceUnavailable ||
               code_ == ScreenError::kSensitivePage || code_ == ScreenError::kCaptureFailed ||
               code_ == ScreenError::kInvalidRequest;
    }

private:
    ScreenError code_;
};

// Convenience aliases matching the prototype semantics.
class ScreenInputError : public ScreenException {
public:
    explicit ScreenInputError(ScreenError code, const std::string &message)
        : ScreenException(code, message) {}
};

class ScreenModelError : public ScreenException {
public:
    explicit ScreenModelError(ScreenError code, const std::string &message)
        : ScreenException(code, message) {}
};

// Map an error code to a stable string identifier for IPC / logging.
const char *ScreenErrorName(ScreenError code);

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_COMMON_SCREEN_ERROR_H
