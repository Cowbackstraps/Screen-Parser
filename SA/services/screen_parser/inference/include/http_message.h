/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Pure-logic HTTP/1.1 message helpers used by the remote VLM transport.
//
// These functions carry no socket / OpenHarmony dependency so they can be
// unit-tested off-device: absolute URL parsing, request serialization, and
// response parsing (both Content-Length and chunked transfer-encoding). The
// actual socket I/O lives in remote_vlm_engine.cpp behind IHttpTransport.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_HTTP_MESSAGE_H
#define FOUNDATION_SCREENPARSER_INFERENCE_HTTP_MESSAGE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OHOS {
namespace ScreenParser {

struct ParsedUrl {
    bool valid = false;
    std::string scheme;   // "http" | "https" (lower-cased)
    std::string host;     // host without port or userinfo
    int32_t port = 0;     // explicit port, or 80/443 default
    std::string path;     // begins with '/', includes the query when present
    bool secure = false;  // true for https
};

// Parse an absolute http(s) URL. Returns valid=false on malformed input
// (missing scheme, empty host, bad port, unsupported scheme).
ParsedUrl ParseUrl(const std::string &url);

using HttpHeader = std::pair<std::string, std::string>;
using HttpHeaders = std::vector<HttpHeader>;

struct HttpRequest {
    std::string method = "POST";
    std::string path = "/";      // request target: path + optional query
    std::string host;            // Host header value, e.g. "127.0.0.1:8000"
    HttpHeaders headers;         // extra headers (Host/Content-Length added for you)
    std::string body;
};

// Serialize an HttpRequest into HTTP/1.1 wire bytes. Host, Content-Length,
// Accept and "Connection: close" are always emitted; a JSON Content-Type is
// added when a body is present and the caller did not set one.
std::string BuildHttpRequest(const HttpRequest &request);

struct HttpResponse {
    int32_t status = 0;
    std::string reason;
    HttpHeaders headers;
    std::string body;            // already de-chunked

    // Case-insensitive header lookup; empty string when absent.
    std::string Header(const std::string &key) const;
};

// Parse a complete raw HTTP response (status line + headers + body). Handles
// Content-Length and chunked transfer-encoding. Returns false when the bytes
// are not a well-formed response (error describes why).
bool ParseHttpResponse(const std::string &raw, HttpResponse &out, std::string &error);

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_HTTP_MESSAGE_H
