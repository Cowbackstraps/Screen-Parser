/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Live transport tests: these run the REAL production SocketHttpTransport over
// actual TCP against an in-process mock OpenAI-compatible server. Unlike the
// fake-transport tests (test_remote_vlm.cpp), they exercise the socket client,
// wire serialization, response framing and the analyzer orchestration end to
// end on the dev machine — no device, no OpenHarmony SDK, no network egress.
//
// Enabled because the host build defines SCREENPARSER_ENABLE_REMOTE_VLM (see
// test/CMakeLists.txt); the socket primitives are portable via socket_compat.h.

#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "http_message.h"
#include "i_vlm_engine.h"
#include "json.h"
#include "remote_vlm_engine.h"
#include "screen_analyzer.h"
#include "screen_capture.h"
#include "socket_compat.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;
namespace sc = socketcompat;

// ---- Helpers --------------------------------------------------------------

// Wrap assistant text into an OpenAI chat-completions response body.
std::string OpenAiResponse(const std::string &content) {
    Value message = Value::MakeObject();
    message.set("role", Value("assistant"));
    message.set("content", Value(content));
    Value choice = Value::MakeObject();
    choice.set("message", message);
    Value choices = Value::MakeArray();
    choices.push_back(choice);
    Value root = Value::MakeObject();
    root.set("choices", choices);
    return root.dump();
}

// Synthetic 16x16 opaque screen (small keeps preprocessing fast).
CapturedScreen MakeScreen() {
    CapturedScreen screen;
    screen.width = 16;
    screen.height = 16;
    screen.isSensitive = false;
    screen.rgba.assign(static_cast<size_t>(16 * 16 * 4), 0);
    for (size_t i = 0; i + 3 < screen.rgba.size(); i += 4) {
        screen.rgba[i] = 120;
        screen.rgba[i + 1] = 200;
        screen.rgba[i + 2] = 80;
        screen.rgba[i + 3] = 255;
    }
    return screen;
}

std::string ToLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// ---- In-process mock OpenAI-compatible HTTP server ------------------------
//
// One-shot: binds an ephemeral loopback port, accepts a single connection,
// records the request (method / path / headers / body), replies with a canned
// chat-completions payload and closes. Uses the same portable socket shim as
// the production transport, so it compiles on both host and device toolchains.
class MockOpenAiServer {
public:
    explicit MockOpenAiServer(std::string responseBody, int status = 200)
        : responseBody_(std::move(responseBody)), status_(status) {}

    ~MockOpenAiServer() { Stop(); }

    bool Start() {
        sc::EnsureSockets();
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!sc::SockValid(listen_)) {
            return false;
        }
        sc::SetReuseAddr(listen_);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listen_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            sc::CloseSock(listen_);
            listen_ = sc::kInvalidSock;
            return false;
        }
        port_ = sc::LocalPort(listen_);
        if (port_ == 0) {
            sc::CloseSock(listen_);
            listen_ = sc::kInvalidSock;
            return false;
        }
        if (::listen(listen_, 4) < 0) {
            sc::CloseSock(listen_);
            listen_ = sc::kInvalidSock;
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this]() { AcceptOnce(); });
        return true;
    }

    void Stop() {
        bool wasRunning = running_.exchange(false);
        if (wasRunning && sc::SockValid(listen_)) {
            sc::ShutdownSock(listen_);
            sc::CloseSock(listen_);
            listen_ = sc::kInvalidSock;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int Port() const { return port_; }
    std::string BaseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

    // Case-insensitive lookup of a recorded request header.
    std::string Header(const std::string &key) const {
        std::string want = ToLower(key);
        for (const auto &h : headers) {
            if (ToLower(h.first) == want) {
                return h.second;
            }
        }
        return std::string();
    }

    // Recorded request.
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

private:
    void AcceptOnce() {
        sc::SockFd c = ::accept(listen_, nullptr, nullptr);
        if (!sc::SockValid(c)) {
            return;
        }
        Handle(c);
        sc::CloseSock(c);
    }

    void Handle(sc::SockFd c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = sc::RecvSock(c, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 8u * 1024u * 1024u) {
                return;
            }
        }
        size_t headerEnd = req.find("\r\n\r\n");
        std::string head = req.substr(0, headerEnd);
        std::string bodyPart = req.substr(headerEnd + 4);

        size_t contentLength = 0;
        std::istringstream hs(head);
        std::string line;
        bool first = true;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (first) {
                first = false;
                std::istringstream requestLine(line);
                requestLine >> method >> path;
                continue;
            }
            size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            headers.emplace_back(key, value);
            if (ToLower(key) == "content-length") {
                contentLength = static_cast<size_t>(std::stoul(value));
            }
        }
        while (bodyPart.size() < contentLength) {
            int n = sc::RecvSock(c, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            bodyPart.append(buf, static_cast<size_t>(n));
        }
        body = bodyPart;

        const char *reason = (status_ >= 200 && status_ < 300) ? "OK" : "Internal Server Error";
        std::ostringstream head2;
        head2 << "HTTP/1.1 " << status_ << " " << reason << "\r\n"
              << "Content-Type: application/json\r\n"
              << "Content-Length: " << responseBody_.size() << "\r\n"
              << "Connection: close\r\n\r\n";
        std::string resp = head2.str() + responseBody_;
        size_t sent = 0;
        while (sent < resp.size()) {
            int n = sc::SendSock(c, resp.data() + sent, resp.size() - sent);
            if (n <= 0) {
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }

    std::string responseBody_;
    int status_;
    sc::SockFd listen_ = sc::kInvalidSock;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

EngineConfig MakeConfig(const std::string &baseUrl, const std::string &apiKey) {
    EngineConfig cfg;
    cfg.backend = VlmBackend::kRemote;
    cfg.baseUrl = baseUrl;
    cfg.model = "qwen2-vl";
    cfg.apiKey = apiKey;
    cfg.timeoutMs = 5000;
    return cfg;
}

// ---- Live round-trip ------------------------------------------------------

TEST(LiveRemoteTest, TransportRoundTripOverRealSocket) {
    MockOpenAiServer server(OpenAiResponse("LIVE_RESULT"));
    ASSERT_TRUE(server.Start());

    RemoteVlmEngine engine;  // default transport == real SocketHttpTransport
    std::string err;
    ASSERT_TRUE(engine.Load(MakeConfig(server.BaseUrl(), "SECRET"), err));

    GenerateRequest req;
    req.systemPrompt = "SYS";
    req.userText = "USER";
    req.imageDataUri = "data:image/png;base64,QUJD";
    std::string out;
    ASSERT_TRUE(engine.Generate(req, out, err));
    EXPECT_EQ(out, std::string("LIVE_RESULT"));

    server.Stop();
    EXPECT_EQ(server.method, std::string("POST"));
    EXPECT_EQ(server.path, std::string("/v1/chat/completions"));
    EXPECT_EQ(server.Header("Authorization"), std::string("Bearer SECRET"));
    EXPECT_EQ(server.Header("Content-Type"), std::string("application/json"));

    Value body;
    std::string pe;
    ASSERT_TRUE(Value::Parse(server.body, body, pe));
    EXPECT_EQ(body.at("model").as_string(), std::string("qwen2-vl"));
    const Value &content = body.at("messages").items()[1].at("content");
    ASSERT_TRUE(content.is_array());
    EXPECT_EQ(content.items()[1].at("image_url").at("url").as_string(),
              std::string("data:image/png;base64,QUJD"));
}

TEST(LiveRemoteTest, AnalyzerEndToEndOverRealSocket) {
    const std::string analysis = R"({
        "page": {"title": "设置", "page_type": "settings", "summary": "系统设置"},
        "nodes": [
            {"id": "wifi", "label": "无线局域网", "role": "list_item",
             "bounds": [40, 100, 960, 180], "confidence": 0.94,
             "interactive": true, "evidence": "vision"}
        ]
    })";
    MockOpenAiServer server(OpenAiResponse(analysis));
    ASSERT_TRUE(server.Start());

    EngineConfig cfg = MakeConfig(server.BaseUrl(), "EMPTY");
    RemoteVlmEngine engine;  // real transport over real TCP
    std::string err;
    ASSERT_TRUE(engine.Load(cfg, err));

    ScreenAnalyzerConfig acfg;
    acfg.engine = cfg;
    ScreenAnalyzer analyzer(&engine, acfg);
    analyzer.SetCaptureFunc([]() { return MakeScreen(); });
    analyzer.SetCurrentAppFunc([]() { return std::string("设置"); });
    const std::string preview = "data:image/png;base64,PREVIEWDATA";
    analyzer.SetPreviewFunc([preview](const CapturedScreen &) { return preview; });

    Value result = analyzer.Analyze();
    EXPECT_EQ(result.at("page").at("page_type").as_string(), std::string("settings"));
    ASSERT_TRUE(result.at("nodes").is_array());
    EXPECT_EQ(result.at("nodes").size(), static_cast<size_t>(1));
    EXPECT_EQ(result.at("screenshot").as_string(), preview);

    server.Stop();
    // The mock server must have really received the preview as the image_url.
    Value body;
    std::string pe;
    ASSERT_TRUE(Value::Parse(server.body, body, pe));
    const Value &content = body.at("messages").items()[1].at("content");
    ASSERT_TRUE(content.is_array());
    EXPECT_EQ(content.items()[1].at("image_url").at("url").as_string(), preview);

    Value status = analyzer.Status();
    EXPECT_EQ(status.at("backend").as_string(), std::string("remote"));
    EXPECT_EQ(status.at("model").as_string(), std::string("qwen2-vl"));
    EXPECT_TRUE(status.at("model_ready").as_bool());
}

TEST(LiveRemoteTest, TransportPropagatesHttpErrorOverRealSocket) {
    MockOpenAiServer server("{\"error\":{\"message\":\"boom\"}}", 500);
    ASSERT_TRUE(server.Start());

    RemoteVlmEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Load(MakeConfig(server.BaseUrl(), "SECRET"), err));

    GenerateRequest req;
    req.imageDataUri = "data:image/png;base64,QUJD";
    std::string out;
    EXPECT_FALSE(engine.Generate(req, out, err));
    EXPECT_TRUE(err.find("500") != std::string::npos);
    server.Stop();
}

TEST(LiveRemoteTest, TransportReportsConnectFailure) {
    // Port 1 on loopback is not listening: connect() must fail fast and surface
    // a transport error rather than hang or crash.
    RemoteVlmEngine engine;
    EngineConfig cfg;
    cfg.backend = VlmBackend::kRemote;
    cfg.baseUrl = "http://127.0.0.1:1/v1";
    cfg.model = "qwen2-vl";
    cfg.timeoutMs = 2000;
    std::string err;
    ASSERT_TRUE(engine.Load(cfg, err));  // Load only validates the URL

    GenerateRequest req;
    req.imageDataUri = "data:image/png;base64,QUJD";
    std::string out;
    EXPECT_FALSE(engine.Generate(req, out, err));
    EXPECT_FALSE(err.empty());
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
