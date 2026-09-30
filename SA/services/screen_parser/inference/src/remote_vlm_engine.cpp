/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "remote_vlm_engine.h"

#include <algorithm>

#if defined(SCREENPARSER_ENABLE_REMOTE_VLM)
// Cross-platform BSD-socket shims (POSIX on device, Winsock2 on host builds).
#include "socket_compat.h"
#endif

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

}  // namespace

// ---- Pure logic (always compiled, unit-testable) --------------------------

std::string RemoteVlmEngine::CompletionsUrl(const std::string &baseUrl) {
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    const std::string suffix = "/chat/completions";
    if (base.size() >= suffix.size() &&
        LowerAscii(base.substr(base.size() - suffix.size())) == suffix) {
        return base;
    }
    return base + suffix;
}

std::string RemoteVlmEngine::BuildRequestBody(const EngineConfig &config,
                                              const GenerateRequest &request) {
    json::Value root = json::Value::MakeObject();
    root.set("model", json::Value(config.model));

    json::Value messages = json::Value::MakeArray();

    json::Value systemMessage = json::Value::MakeObject();
    systemMessage.set("role", json::Value("system"));
    systemMessage.set("content", json::Value(request.systemPrompt));
    messages.push_back(systemMessage);

    json::Value userMessage = json::Value::MakeObject();
    userMessage.set("role", json::Value("user"));
    json::Value content = json::Value::MakeArray();

    json::Value textPart = json::Value::MakeObject();
    textPart.set("type", json::Value("text"));
    textPart.set("text", json::Value(request.userText));
    content.push_back(textPart);

    if (!request.imageDataUri.empty()) {
        json::Value imagePart = json::Value::MakeObject();
        imagePart.set("type", json::Value("image_url"));
        json::Value imageUrl = json::Value::MakeObject();
        imageUrl.set("url", json::Value(request.imageDataUri));
        imagePart.set("image_url", imageUrl);
        content.push_back(imagePart);
    }

    userMessage.set("content", content);
    messages.push_back(userMessage);
    root.set("messages", messages);

    int32_t maxTokens = request.maxTokens > 0 ? request.maxTokens : config.maxTokens;
    root.set("max_tokens", json::Value(maxTokens));
    root.set("temperature", json::Value(static_cast<double>(request.temperature)));
    root.set("stream", json::Value(false));
    return root.dump();
}

bool RemoteVlmEngine::ExtractAssistantText(const std::string &responseBody, std::string &outText,
                                           std::string &error) {
    outText.clear();
    json::Value parsed;
    std::string parseError;
    if (!json::Value::Parse(responseBody, parsed, parseError) || !parsed.is_object()) {
        error = "远程响应不是合法 JSON: " + parseError;
        return false;
    }

    // OpenAI-style error object: { "error": { "message": ... } } or a string.
    if (parsed.contains("error")) {
        const json::Value &err = parsed.at("error");
        std::string msg = err.is_object() ? err.at("message").as_string_or("远程模型返回错误")
                                          : err.as_string_or("远程模型返回错误");
        error = "远程模型错误: " + msg;
        return false;
    }

    const json::Value &choices = parsed.at("choices");
    if (!choices.is_array() || choices.size() == 0) {
        error = "远程响应缺少 choices";
        return false;
    }
    const json::Value &first = choices.items()[0];
    const json::Value &message = first.at("message");
    std::string content = message.at("content").as_string_or("");
    if (content.empty()) {
        error = "远程模型返回空内容";
        return false;
    }
    outText = content;
    return true;
}

// ---- Engine lifecycle -----------------------------------------------------

RemoteVlmEngine::RemoteVlmEngine(IHttpTransport *transport) : transport_(transport) {}

RemoteVlmEngine::~RemoteVlmEngine() = default;

bool RemoteVlmEngine::Load(const EngineConfig &config, std::string &error) {
    config_ = config;
    ready_ = false;

    if (config_.baseUrl.empty()) {
        error = "远程 VLM 缺少 baseUrl";
        return false;
    }
    ParsedUrl url = ParseUrl(config_.baseUrl);
    if (!url.valid) {
        error = "远程 VLM baseUrl 非法: " + config_.baseUrl;
        return false;
    }
    if (url.secure) {
        // The bundled socket transport speaks plain http:// only; https needs a
        // TLS backend (netstack). Fail fast with an actionable message.
        error = "远程 VLM 暂不支持 https（请使用 http:// 或提供 TLS 传输）";
        return false;
    }

    if (transport_ == nullptr) {
        ownedTransport_ = MakeDefaultHttpTransport();
        transport_ = ownedTransport_.get();
    }
    if (transport_ == nullptr) {
        error = "无可用 HTTP 传输";
        return false;
    }

    ready_ = true;
    return true;
}

bool RemoteVlmEngine::Generate(const GenerateRequest &request, std::string &outText,
                               std::string &error) {
    outText.clear();
    if (!ready_ || transport_ == nullptr) {
        error = "远程 VLM 未就绪";
        return false;
    }
    if (request.imageDataUri.empty()) {
        error = "远程 VLM 需要图像数据(imageDataUri)";
        return false;
    }

    std::string url = CompletionsUrl(config_.baseUrl);
    HttpHeaders headers;
    headers.emplace_back("Content-Type", "application/json");
    if (!config_.apiKey.empty()) {
        headers.emplace_back("Authorization", "Bearer " + config_.apiKey);
    }
    std::string body = BuildRequestBody(config_, request);

    HttpResponse response;
    if (!transport_->Post(url, headers, body, config_.timeoutMs, response, error)) {
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        std::string snippet = response.body.substr(0, 200);
        error = "远程 VLM HTTP " + std::to_string(response.status);
        if (!snippet.empty()) {
            error += ": " + snippet;
        }
        return false;
    }
    return ExtractAssistantText(response.body, outText, error);
}

// ---- Default transport ----------------------------------------------------

#if defined(SCREENPARSER_ENABLE_REMOTE_VLM)

namespace {

// Minimal blocking HTTP/1.1 client over BSD sockets (plain http://). We always
// send "Connection: close" and read until EOF, so no keep-alive bookkeeping is
// required. SO_RCVTIMEO/SO_SNDTIMEO bound each syscall so a stalled peer cannot
// hang the service thread.
class SocketHttpTransport : public IHttpTransport {
public:
    bool Post(const std::string &url, const HttpHeaders &headers, const std::string &body,
              int32_t timeoutMs, HttpResponse &response, std::string &error) override {
        socketcompat::EnsureSockets();
        ParsedUrl parsed = ParseUrl(url);
        if (!parsed.valid) {
            error = "非法 URL: " + url;
            return false;
        }
        if (parsed.secure) {
            error = "socket 传输不支持 https";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        std::string portStr = std::to_string(parsed.port);
        if (::getaddrinfo(parsed.host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
            error = "域名解析失败: " + parsed.host;
            return false;
        }

        socketcompat::SockFd fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (!socketcompat::SockValid(fd)) {
            ::freeaddrinfo(res);
            error = "创建 socket 失败";
            return false;
        }

        int32_t effectiveTimeout = timeoutMs > 0 ? timeoutMs : 30000;
        socketcompat::SetTimeouts(fd, effectiveTimeout);

        if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            ::freeaddrinfo(res);
            socketcompat::CloseSock(fd);
            error = std::string("连接失败: ") + socketcompat::LastSockError();
            return false;
        }
        ::freeaddrinfo(res);

        HttpRequest req;
        req.method = "POST";
        req.path = parsed.path;
        req.host = parsed.host;
        if (parsed.port != 80) {
            req.host += ":" + std::to_string(parsed.port);
        }
        req.headers = headers;
        req.body = body;
        std::string wire = BuildHttpRequest(req);

        if (!SendAll(fd, wire)) {
            socketcompat::CloseSock(fd);
            error = "发送请求失败";
            return false;
        }

        std::string raw;
        if (!RecvAll(fd, raw)) {
            socketcompat::CloseSock(fd);
            error = "接收响应失败或超时";
            return false;
        }
        socketcompat::CloseSock(fd);

        return ParseHttpResponse(raw, response, error);
    }

private:
    static bool SendAll(socketcompat::SockFd fd, const std::string &data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = socketcompat::SendSock(fd, data.data() + sent, data.size() - sent);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Read until the peer closes the connection (Connection: close) or a
    // receive timeout fires. Returns false when nothing was received.
    static bool RecvAll(socketcompat::SockFd fd, std::string &out) {
        out.clear();
        char buf[8192];
        while (true) {
            int n = socketcompat::RecvSock(fd, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                if (out.size() > 64u * 1024u * 1024u) {
                    return false;  // runaway guard (64 MB)
                }
                continue;
            }
            break;  // n == 0 (EOF) or n < 0 (timeout / error via SO_RCVTIMEO)
        }
        return !out.empty();
    }
};

}  // namespace

std::unique_ptr<IHttpTransport> MakeDefaultHttpTransport() {
    return std::make_unique<SocketHttpTransport>();
}

#else  // !SCREENPARSER_ENABLE_REMOTE_VLM

namespace {

// Host / disabled build: no network. Post() always fails so the analyzer
// surfaces a clear "remote VLM unavailable" error instead of hanging.
class StubHttpTransport : public IHttpTransport {
public:
    bool Post(const std::string &, const HttpHeaders &, const std::string &, int32_t,
              HttpResponse &, std::string &error) override {
        error = "远程 VLM 传输未在此构建中启用（SCREENPARSER_ENABLE_REMOTE_VLM）";
        return false;
    }
};

}  // namespace

std::unique_ptr<IHttpTransport> MakeDefaultHttpTransport() {
    return std::make_unique<StubHttpTransport>();
}

#endif  // SCREENPARSER_ENABLE_REMOTE_VLM

}  // namespace ScreenParser
}  // namespace OHOS
