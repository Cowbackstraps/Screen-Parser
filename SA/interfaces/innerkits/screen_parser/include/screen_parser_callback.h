/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Callback plumbing for AnalyzeAsync. Clients subclass ScreenParserCallbackStub
// to receive results; the service uses ScreenParserCallbackProxy transparently
// through iface_cast when the callback lives in another process.

#ifndef FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_CALLBACK_H
#define FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_CALLBACK_H

#include "i_screen_parser.h"
#include "iremote_proxy.h"
#include "iremote_stub.h"

namespace OHOS {
namespace ScreenParser {

class ScreenParserCallbackStub : public IRemoteStub<IScreenParserCallback> {
public:
    ScreenParserCallbackStub() = default;
    ~ScreenParserCallbackStub() override = default;

    int OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                        MessageOption &option) override;
};

class ScreenParserCallbackProxy : public IRemoteProxy<IScreenParserCallback> {
public:
    explicit ScreenParserCallbackProxy(const sptr<IRemoteObject> &impl)
        : IRemoteProxy<IScreenParserCallback>(impl) {}
    ~ScreenParserCallbackProxy() override = default;

    int32_t OnResult(const std::string &resultJson) override;
    int32_t OnError(int32_t code, const std::string &message) override;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_CALLBACK_H
