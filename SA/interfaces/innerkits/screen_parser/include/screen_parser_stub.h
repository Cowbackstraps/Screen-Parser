/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_STUB_H
#define FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_STUB_H

#include "i_screen_parser.h"
#include "iremote_stub.h"

namespace OHOS {
namespace ScreenParser {

class ScreenParserStub : public IRemoteStub<IScreenParser> {
public:
    ScreenParserStub() = default;
    virtual ~ScreenParserStub() = default;

    int OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                        MessageOption &option) override;

private:
    void HandleGetStatus(MessageParcel &data, MessageParcel &reply);
    void HandleAnalyzeSync(MessageParcel &data, MessageParcel &reply);
    void HandleAnalyzeAsync(MessageParcel &data, MessageParcel &reply);
    void HandleRecognizeText(MessageParcel &data, MessageParcel &reply);

    static constexpr int32_t kErrInvalidState = -3;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_STUB_H
