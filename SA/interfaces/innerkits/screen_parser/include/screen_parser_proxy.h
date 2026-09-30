/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_PROXY_H
#define FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_PROXY_H

#include "i_screen_parser.h"
#include "iremote_proxy.h"

namespace OHOS {
namespace ScreenParser {

class ScreenParserProxy : public IRemoteProxy<IScreenParser> {
public:
    explicit ScreenParserProxy(const sptr<IRemoteObject> &impl)
        : IRemoteProxy<IScreenParser>(impl) {}
    ~ScreenParserProxy() override = default;

    std::string GetStatus() override;
    std::string AnalyzeSync(int32_t timeoutMs) override;
    int32_t AnalyzeAsync(const sptr<IScreenParserCallback> &callback) override;
    std::string RecognizeText(int32_t timeoutMs) override;

private:
    // Reads a string transported either inline or via ashmem (see stub).
    static std::string ReadLargeString(MessageParcel &reply);
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INTERFACES_SCREEN_PARSER_PROXY_H
