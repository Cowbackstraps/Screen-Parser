/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "http_message.h"

#include <algorithm>
#include <cstdlib>

namespace OHOS {
namespace ScreenParser {

namespace {

std::string LowerAscii(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// Split on '\n', stripping a trailing '\r' from each line.
std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            std::string line = text.substr(start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
            break;
        }
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        start = nl + 1;
    }
    return lines;
}

// Decode a "Transfer-Encoding: chunked" body into plain bytes.
bool DecodeChunked(const std::string &body, std::string &out) {
    out.clear();
    size_t pos = 0;
    while (pos < body.size()) {
        size_t lineEnd = body.find("\r\n", pos);
        size_t advance = 2;
        if (lineEnd == std::string::npos) {
            lineEnd = body.find('\n', pos);
            advance = 1;
            if (lineEnd == std::string::npos) {
                break;  // incomplete size line
            }
        }
        std::string sizeLine = body.substr(pos, lineEnd - pos);
        size_t semi = sizeLine.find(';');  // strip chunk extensions
        if (semi != std::string::npos) {
            sizeLine = sizeLine.substr(0, semi);
        }
        if (sizeLine.empty()) {
            return false;
        }
        unsigned long chunkSize = std::strtoul(sizeLine.c_str(), nullptr, 16);
        pos = lineEnd + advance;
        if (chunkSize == 0) {
            break;  // terminating chunk
        }
        if (pos + chunkSize > body.size()) {
            out.append(body, pos, body.size() - pos);  // truncated tail
            break;
        }
        out.append(body, pos, chunkSize);
        pos += chunkSize;
        if (pos < body.size() && body[pos] == '\r') {
            ++pos;
        }
        if (pos < body.size() && body[pos] == '\n') {
            ++pos;
        }
    }
    return true;
}

}  // namespace

ParsedUrl ParseUrl(const std::string &url) {
    ParsedUrl result;
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) {
        return result;
    }
    result.scheme = LowerAscii(url.substr(0, schemeEnd));
    if (result.scheme != "http" && result.scheme != "https") {
        return result;
    }
    result.secure = (result.scheme == "https");
    result.port = result.secure ? 443 : 80;

    std::string rest = url.substr(schemeEnd + 3);
    if (rest.empty()) {
        return result;
    }

    std::string authority = rest;
    std::string pathAndQuery = "/";
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        authority = rest.substr(0, slash);
        pathAndQuery = rest.substr(slash);
    }
    if (authority.empty()) {
        return result;
    }

    size_t at = authority.find('@');  // strip userinfo
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }

    std::string host = authority;
    size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        std::string portStr = authority.substr(colon + 1);
        if (portStr.empty()) {
            return result;
        }
        int32_t port = 0;
        for (char c : portStr) {
            if (c < '0' || c > '9') {
                return result;
            }
            port = port * 10 + (c - '0');
            if (port > 65535) {
                return result;
            }
        }
        if (port <= 0) {
            return result;
        }
        result.port = port;
    }
    if (host.empty()) {
        return result;
    }

    result.host = host;
    result.path = pathAndQuery.empty() ? std::string("/") : pathAndQuery;
    result.valid = true;
    return result;
}

std::string BuildHttpRequest(const HttpRequest &request) {
    std::string out;
    out += request.method.empty() ? std::string("GET") : request.method;
    out += " ";
    out += request.path.empty() ? std::string("/") : request.path;
    out += " HTTP/1.1\r\n";
    if (!request.host.empty()) {
        out += "Host: " + request.host + "\r\n";
    }

    bool hasContentType = false;
    bool hasAccept = false;
    for (const auto &header : request.headers) {
        out += header.first + ": " + header.second + "\r\n";
        std::string key = LowerAscii(header.first);
        if (key == "content-type") {
            hasContentType = true;
        } else if (key == "accept") {
            hasAccept = true;
        }
    }
    if (!hasContentType && !request.body.empty()) {
        out += "Content-Type: application/json\r\n";
    }
    if (!hasAccept) {
        out += "Accept: application/json\r\n";
    }
    out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "\r\n";
    out += request.body;
    return out;
}

std::string HttpResponse::Header(const std::string &key) const {
    std::string want = LowerAscii(key);
    for (const auto &header : headers) {
        if (LowerAscii(header.first) == want) {
            return header.second;
        }
    }
    return std::string();
}

bool ParseHttpResponse(const std::string &raw, HttpResponse &out, std::string &error) {
    out = HttpResponse{};

    size_t headerEnd = raw.find("\r\n\r\n");
    size_t separator = 4;
    if (headerEnd == std::string::npos) {
        headerEnd = raw.find("\n\n");
        separator = 2;
        if (headerEnd == std::string::npos) {
            error = "响应缺少头部结束标记";
            return false;
        }
    }
    std::string head = raw.substr(0, headerEnd);
    std::string body = raw.substr(headerEnd + separator);

    std::vector<std::string> lines = SplitLines(head);
    if (lines.empty() || lines[0].empty()) {
        error = "空响应";
        return false;
    }

    // Status line: "HTTP/1.1 200 OK".
    const std::string &statusLine = lines[0];
    size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) {
        error = "非法状态行";
        return false;
    }
    size_t sp2 = statusLine.find(' ', sp1 + 1);
    std::string codeStr = (sp2 == std::string::npos) ? statusLine.substr(sp1 + 1)
                                                     : statusLine.substr(sp1 + 1, sp2 - sp1 - 1);
    if (codeStr.empty()) {
        error = "非法状态码";
        return false;
    }
    int32_t status = 0;
    for (char c : codeStr) {
        if (c < '0' || c > '9') {
            error = "非法状态码";
            return false;
        }
        status = status * 10 + (c - '0');
    }
    if (status <= 0) {
        error = "非法状态码";
        return false;
    }
    out.status = status;
    if (sp2 != std::string::npos) {
        out.reason = statusLine.substr(sp2 + 1);
    }

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        size_t b = value.find_first_not_of(" \t");
        value = (b == std::string::npos) ? std::string() : value.substr(b);
        out.headers.emplace_back(key, value);
    }

    std::string transferEncoding = LowerAscii(out.Header("Transfer-Encoding"));
    if (transferEncoding.find("chunked") != std::string::npos) {
        if (!DecodeChunked(body, out.body)) {
            error = "分块响应解码失败";
            return false;
        }
        return true;
    }

    std::string contentLength = out.Header("Content-Length");
    if (!contentLength.empty()) {
        size_t n = static_cast<size_t>(std::strtoul(contentLength.c_str(), nullptr, 10));
        out.body = body.substr(0, std::min(n, body.size()));
    } else {
        out.body = body;
    }
    return true;
}

}  // namespace ScreenParser
}  // namespace OHOS
