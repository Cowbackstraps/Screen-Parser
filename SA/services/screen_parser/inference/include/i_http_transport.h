/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Transport abstraction for the remote VLM backend.
//
// IHttpTransport performs a single blocking HTTP POST and returns the parsed
// response. RemoteVlmEngine depends only on this interface, so unit tests can
// inject a fake transport (canned response) and verify the entire
// request/response pipeline offline. The production SocketHttpTransport is
// compiled only when SCREENPARSER_ENABLE_REMOTE_VLM is set (BSD sockets on
// device); host builds get a stub whose Post() always fails.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_I_HTTP_TRANSPORT_H
#define FOUNDATION_SCREENPARSER_INFERENCE_I_HTTP_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>

#include "http_message.h"

namespace OHOS {
namespace ScreenParser {

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    // POST `body` to the absolute `url` with the given headers. Returns true
    // when a response was received (response populated, whatever the HTTP
    // status); false on a transport-level failure (DNS / connect / timeout /
    // TLS), with error set. `timeoutMs` bounds the exchange (<=0 -> default).
    virtual bool Post(const std::string &url, const HttpHeaders &headers,
                      const std::string &body, int32_t timeoutMs,
                      HttpResponse &response, std::string &error) = 0;
};

// Create the default transport for this build:
//  * SCREENPARSER_ENABLE_REMOTE_VLM defined -> BSD-socket HTTP client (http://).
//  * otherwise -> a stub whose Post() always fails (host unit-test builds).
std::unique_ptr<IHttpTransport> MakeDefaultHttpTransport();

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_I_HTTP_TRANSPORT_H
