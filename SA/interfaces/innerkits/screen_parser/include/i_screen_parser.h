/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// IPC contract of the screen parser system ability.
//
// Clients obtain a proxy via SystemAbilityManager (SAID kScreenParserSaId) and
// call these methods. Results are JSON strings produced by ScreenAnalyzer.

#ifndef FOUNDATION_SCREENPARSER_INTERFACES_I_SCREEN_PARSER_H
#define FOUNDATION_SCREENPARSER_INTERFACES_I_SCREEN_PARSER_H

#include <cstdint>
#include <string>

#include "iremote_broker.h"

namespace OHOS {
namespace ScreenParser {

// Vendor/custom System Ability ID.
//
// OpenHarmony reserves the range [VENDOR_SYS_ABILITY_ID_BEGIN (0x10000 = 65536),
// VENDOR_SYS_ABILITY_ID_END (0x20000 = 131072)] for downstream/vendor components;
// official subsystem SAIDs (samgr system_ability_definition.h) must NOT be reused.
// The earlier provisional value 4801 was rejected because it collides with the
// official DISTRIBUTED_HARDWARE_SA_ID. 65537 = VENDOR_SYS_ABILITY_ID_BEGIN + 1 is
// the first free vendor slot. REGISTER_SYSTEM_ABILITY_BY_ID takes this literal, so
// no edit to system_ability_definition.h is required.
// Keep in sync with <name> in sa_profile/screenparser.xml.
constexpr int32_t kScreenParserSaId = 65537;

class IScreenParserCallback : public IRemoteBroker {
public:
    DECLARE_INTERFACE_DESCRIPTOR(u"OHOS.ScreenParser.Callback");

    enum class Message : uint32_t {
        ON_RESULT = 0,
        ON_ERROR = 1,
    };

    // Delivered with the analysis JSON on success.
    virtual int32_t OnResult(const std::string &resultJson) = 0;
    // Delivered with a ScreenError code and message on failure.
    virtual int32_t OnError(int32_t code, const std::string &message) = 0;
};

class IScreenParser : public IRemoteBroker {
public:
    DECLARE_INTERFACE_DESCRIPTOR(u"OHOS.ScreenParser");

    enum class Message : uint32_t {
        GET_STATUS = 0,
        ANALYZE_SYNC = 1,
        ANALYZE_ASYNC = 2,
        RECOGNIZE_TEXT = 3,
    };

    // JSON: devices / default_device / model / model_ready.
    virtual std::string GetStatus() = 0;

    // Blocking analysis. timeoutMs <= 0 uses the service default (60s).
    // Returns the analysis JSON, or throws/returns an error payload.
    virtual std::string AnalyzeSync(int32_t timeoutMs) = 0;

    // Asynchronous analysis; results are delivered through the callback.
    // Returns 0 on successful dispatch.
    virtual int32_t AnalyzeAsync(const sptr<IScreenParserCallback> &callback) = 0;

    // Text-only recognition using the on-device OCR engine. timeoutMs <= 0 uses
    // the service default. Returns JSON: ocr_text / blocks[] / screen_width /
    // screen_height / device_id, or an error payload.
    virtual std::string RecognizeText(int32_t timeoutMs) = 0;

    enum class Error : int32_t {
        kOk = 0,
        kInvalidArgument = -1,
        kIpcFailure = -2,
    };
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INTERFACES_I_SCREEN_PARSER_H
