/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Unit tests for the remote VLM backend: the pure-logic HTTP message helpers
// (ParseUrl / BuildHttpRequest / ParseHttpResponse), the OpenAI-compatible
// request/response building in RemoteVlmEngine, and an end-to-end analyzer run
// driven by an injected fake transport so everything executes offline.

#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "http_message.h"
#include "i_http_transport.h"
#include "i_vlm_engine.h"
#include "json.h"
#include "remote_vlm_engine.h"
#include "screen_analyzer.h"
#include "screen_capture.h"

namespace OHOS {
namespace ScreenParser {
namespace {

using json::Value;

// ---- Test doubles ---------------------------------------------------------

// Programmable transport: records the last request and returns a canned reply.
class FakeTransport : public IHttpTransport {
public:
    bool Post(const std::string &url, const HttpHeaders &headers, const std::string &body,
              int32_t timeoutMs, HttpResponse &response, std::string &error) override {
        ++calls;
        lastUrl = url;
        lastHeaders = headers;
        lastBody = body;
        lastTimeout = timeoutMs;
        if (failTransport) {
            error = "simulated transport failure";
            return false;
        }
        response = HttpResponse{};
        response.status = status;
        response.reason = "OK";
        response.body = responseBody;
        return true;
    }

    // Case-insensitive lookup of a recorded request header.
    std::string Header(const std::string &key) const {
        for (const auto &h : lastHeaders) {
            if (h.first.size() != key.size()) {
                continue;
            }
            bool same = true;
            for (size_t i = 0; i < key.size(); ++i) {
                char a = h.first[i];
                char b = key[i];
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = static_cast<char>(b - 'A' + 'a');
                }
                if (a != b) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return h.second;
            }
        }
        return std::string();
    }

    int calls = 0;
    std::string lastUrl;
    HttpHeaders lastHeaders;
    std::string lastBody;
    int32_t lastTimeout = 0;
    bool failTransport = false;
    int32_t status = 200;
    std::string responseBody;
};

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
        screen.rgba[i] = 120;      // R
        screen.rgba[i + 1] = 200;  // G
        screen.rgba[i + 2] = 80;   // B
        screen.rgba[i + 3] = 255;  // A
    }
    return screen;
}

EngineConfig MakeRemoteConfig() {
    EngineConfig cfg;
    cfg.backend = VlmBackend::kRemote;
    cfg.baseUrl = "http://127.0.0.1:8000/v1";
    cfg.model = "qwen2-vl";
    cfg.apiKey = "SECRET";
    cfg.timeoutMs = 12345;
    return cfg;
}

// ---- ParseUrl -------------------------------------------------------------

TEST(HttpMessageTest, ParseUrlHttpWithPort) {
    ParsedUrl u = ParseUrl("http://127.0.0.1:8000/v1");
    EXPECT_TRUE(u.valid);
    EXPECT_EQ(u.scheme, std::string("http"));
    EXPECT_EQ(u.host, std::string("127.0.0.1"));
    EXPECT_EQ(u.port, 8000);
    EXPECT_EQ(u.path, std::string("/v1"));
    EXPECT_FALSE(u.secure);
}

TEST(HttpMessageTest, ParseUrlDefaultsAndQuery) {
    ParsedUrl plain = ParseUrl("http://example.com");
    EXPECT_TRUE(plain.valid);
    EXPECT_EQ(plain.port, 80);
    EXPECT_EQ(plain.path, std::string("/"));

    ParsedUrl secure = ParseUrl("https://api.example.com/v1");
    EXPECT_TRUE(secure.valid);
    EXPECT_TRUE(secure.secure);
    EXPECT_EQ(secure.port, 443);

    ParsedUrl query = ParseUrl("http://h/p?q=1&r=2");
    EXPECT_TRUE(query.valid);
    EXPECT_EQ(query.path, std::string("/p?q=1&r=2"));
}

TEST(HttpMessageTest, ParseUrlRejectsMalformed) {
    EXPECT_FALSE(ParseUrl("ftp://host").valid);
    EXPECT_FALSE(ParseUrl("example.com").valid);
    EXPECT_FALSE(ParseUrl("http://").valid);
    EXPECT_FALSE(ParseUrl("http://h:abc").valid);
    EXPECT_FALSE(ParseUrl("").valid);
}

// ---- BuildHttpRequest -----------------------------------------------------

TEST(HttpMessageTest, BuildHttpRequestSerializesPost) {
    HttpRequest req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.host = "h:1";
    req.body = "{\"a\":1}";
    std::string wire = BuildHttpRequest(req);
    EXPECT_TRUE(wire.find("POST /v1/chat/completions HTTP/1.1\r\n") == 0);
    EXPECT_TRUE(wire.find("Host: h:1\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Content-Type: application/json\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Content-Length: 7\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Connection: close\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("\r\n\r\n{\"a\":1}") != std::string::npos);
}

// ---- ParseHttpResponse ----------------------------------------------------

TEST(HttpMessageTest, ParseHttpResponseContentLength) {
    std::string raw =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nHello";
    HttpResponse resp;
    std::string err;
    ASSERT_TRUE(ParseHttpResponse(raw, resp, err));
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.reason, std::string("OK"));
    EXPECT_EQ(resp.body, std::string("Hello"));
    EXPECT_EQ(resp.Header("content-type"), std::string("text/plain"));
    EXPECT_EQ(resp.Header("CONTENT-LENGTH"), std::string("5"));
    EXPECT_EQ(resp.Header("missing"), std::string(""));
}

TEST(HttpMessageTest, ParseHttpResponseChunked) {
    std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "5\r\nHello\r\n1\r\n \r\n5\r\nWorld\r\n0\r\n\r\n";
    HttpResponse resp;
    std::string err;
    ASSERT_TRUE(ParseHttpResponse(raw, resp, err));
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body, std::string("Hello World"));
}

TEST(HttpMessageTest, ParseHttpResponseRejectsMalformed) {
    HttpResponse resp;
    std::string err;
    EXPECT_FALSE(ParseHttpResponse("garbage without headers", resp, err));
    EXPECT_FALSE(err.empty());
}

// ---- CompletionsUrl -------------------------------------------------------

TEST(RemoteVlmEngineTest, CompletionsUrlAppendsSuffix) {
    EXPECT_EQ(RemoteVlmEngine::CompletionsUrl("http://h/v1"),
              std::string("http://h/v1/chat/completions"));
    EXPECT_EQ(RemoteVlmEngine::CompletionsUrl("http://h/v1/"),
              std::string("http://h/v1/chat/completions"));
    EXPECT_EQ(RemoteVlmEngine::CompletionsUrl("http://h/v1/chat/completions"),
              std::string("http://h/v1/chat/completions"));
}

// ---- BuildRequestBody -----------------------------------------------------

TEST(RemoteVlmEngineTest, BuildRequestBodyOpenAiShape) {
    EngineConfig cfg = MakeRemoteConfig();
    GenerateRequest req;
    req.systemPrompt = "SYS";
    req.userText = "USER";
    req.imageDataUri = "data:image/png;base64,IMG";
    req.maxTokens = 77;
    req.temperature = 0.0f;

    std::string body = RemoteVlmEngine::BuildRequestBody(cfg, req);
    Value parsed;
    std::string err;
    ASSERT_TRUE(Value::Parse(body, parsed, err));
    EXPECT_EQ(parsed.at("model").as_string(), std::string("qwen2-vl"));
    EXPECT_EQ(parsed.at("max_tokens").as_int(0), static_cast<int64_t>(77));
    EXPECT_FALSE(parsed.at("stream").as_bool(true));

    const Value &messages = parsed.at("messages");
    ASSERT_TRUE(messages.is_array());
    EXPECT_EQ(messages.size(), static_cast<size_t>(2));
    EXPECT_EQ(messages.items()[0].at("role").as_string(), std::string("system"));
    EXPECT_EQ(messages.items()[0].at("content").as_string(), std::string("SYS"));
    EXPECT_EQ(messages.items()[1].at("role").as_string(), std::string("user"));

    const Value &content = messages.items()[1].at("content");
    ASSERT_TRUE(content.is_array());
    EXPECT_EQ(content.size(), static_cast<size_t>(2));
    EXPECT_EQ(content.items()[0].at("type").as_string(), std::string("text"));
    EXPECT_EQ(content.items()[0].at("text").as_string(), std::string("USER"));
    EXPECT_EQ(content.items()[1].at("type").as_string(), std::string("image_url"));
    EXPECT_EQ(content.items()[1].at("image_url").at("url").as_string(),
              std::string("data:image/png;base64,IMG"));
}

// ---- ExtractAssistantText -------------------------------------------------

TEST(RemoteVlmEngineTest, ExtractAssistantTextSuccess) {
    std::string out;
    std::string err;
    ASSERT_TRUE(RemoteVlmEngine::ExtractAssistantText(OpenAiResponse("hello"), out, err));
    EXPECT_EQ(out, std::string("hello"));
}

TEST(RemoteVlmEngineTest, ExtractAssistantTextErrorObject) {
    std::string out;
    std::string err;
    EXPECT_FALSE(RemoteVlmEngine::ExtractAssistantText("{\"error\":{\"message\":\"rate limited\"}}",
                                                       out, err));
    EXPECT_TRUE(err.find("rate limited") != std::string::npos);
}

TEST(RemoteVlmEngineTest, ExtractAssistantTextMissingChoices) {
    std::string out;
    std::string err;
    EXPECT_FALSE(RemoteVlmEngine::ExtractAssistantText("{\"id\":\"x\"}", out, err));
    EXPECT_TRUE(err.find("choices") != std::string::npos);
}

TEST(RemoteVlmEngineTest, ExtractAssistantTextEmptyContent) {
    std::string out;
    std::string err;
    EXPECT_FALSE(RemoteVlmEngine::ExtractAssistantText(OpenAiResponse(""), out, err));
    EXPECT_FALSE(err.empty());
}

TEST(RemoteVlmEngineTest, ExtractAssistantTextInvalidJson) {
    std::string out;
    std::string err;
    EXPECT_FALSE(RemoteVlmEngine::ExtractAssistantText("not json", out, err));
    EXPECT_FALSE(err.empty());
}

// ---- Load / Generate ------------------------------------------------------

TEST(RemoteVlmEngineTest, LoadRejectsEmptyAndHttps) {
    FakeTransport fake;
    RemoteVlmEngine engine(&fake);
    std::string err;

    EngineConfig empty;
    empty.backend = VlmBackend::kRemote;
    EXPECT_FALSE(engine.Load(empty, err));
    EXPECT_FALSE(err.empty());

    EngineConfig https;
    https.backend = VlmBackend::kRemote;
    https.baseUrl = "https://api.example.com/v1";
    EXPECT_FALSE(engine.Load(https, err));
    EXPECT_TRUE(err.find("https") != std::string::npos);
    EXPECT_FALSE(engine.IsReady());
}

TEST(RemoteVlmEngineTest, LoadAcceptsHttp) {
    FakeTransport fake;
    RemoteVlmEngine engine(&fake);
    std::string err;
    ASSERT_TRUE(engine.Load(MakeRemoteConfig(), err));
    EXPECT_TRUE(engine.IsReady());
}

TEST(RemoteVlmEngineTest, GenerateSuccessAndRequestShape) {
    FakeTransport fake;
    fake.responseBody = OpenAiResponse("RESULT");
    RemoteVlmEngine engine(&fake);
    std::string err;
    ASSERT_TRUE(engine.Load(MakeRemoteConfig(), err));

    GenerateRequest req;
    req.systemPrompt = "SYS";
    req.userText = "USER";
    req.imageDataUri = "data:image/png;base64,IMG";
    std::string out;
    ASSERT_TRUE(engine.Generate(req, out, err));
    EXPECT_EQ(out, std::string("RESULT"));
    EXPECT_EQ(fake.calls, 1);
    EXPECT_EQ(fake.lastUrl, std::string("http://127.0.0.1:8000/v1/chat/completions"));
    EXPECT_EQ(fake.lastTimeout, 12345);
    EXPECT_EQ(fake.Header("Authorization"), std::string("Bearer SECRET"));
    EXPECT_EQ(fake.Header("Content-Type"), std::string("application/json"));

    Value body;
    std::string perr;
    ASSERT_TRUE(Value::Parse(fake.lastBody, body, perr));
    EXPECT_EQ(body.at("model").as_string(), std::string("qwen2-vl"));
}

TEST(RemoteVlmEngineTest, GeneratePropagatesHttpError) {
    FakeTransport fake;
    fake.status = 500;
    fake.responseBody = "{\"error\":{\"message\":\"boom\"}}";
    RemoteVlmEngine engine(&fake);
    std::string err;
    ASSERT_TRUE(engine.Load(MakeRemoteConfig(), err));

    GenerateRequest req;
    req.imageDataUri = "data:image/png;base64,IMG";
    std::string out;
    EXPECT_FALSE(engine.Generate(req, out, err));
    EXPECT_TRUE(err.find("500") != std::string::npos);
}

TEST(RemoteVlmEngineTest, GeneratePropagatesTransportFailure) {
    FakeTransport fake;
    fake.failTransport = true;
    RemoteVlmEngine engine(&fake);
    std::string err;
    ASSERT_TRUE(engine.Load(MakeRemoteConfig(), err));

    GenerateRequest req;
    req.imageDataUri = "data:image/png;base64,IMG";
    std::string out;
    EXPECT_FALSE(engine.Generate(req, out, err));
    EXPECT_TRUE(err.find("transport") != std::string::npos);
}

TEST(RemoteVlmEngineTest, GenerateRequiresImageDataUri) {
    FakeTransport fake;
    RemoteVlmEngine engine(&fake);
    std::string err;
    ASSERT_TRUE(engine.Load(MakeRemoteConfig(), err));

    GenerateRequest req;  // no imageDataUri
    std::string out;
    EXPECT_FALSE(engine.Generate(req, out, err));
    EXPECT_EQ(fake.calls, 0);  // never hit the network
}

// ---- End-to-end through the analyzer --------------------------------------

TEST(RemoteVlmEngineTest, AnalyzerEndToEndWithRemoteEngine) {
    const std::string analysis = R"({
        "page": {"title": "设置", "page_type": "settings", "summary": "系统设置"},
        "nodes": [
            {"id": "wifi", "label": "无线局域网", "role": "list_item",
             "bounds": [40, 100, 960, 180], "confidence": 0.94,
             "interactive": true, "evidence": "vision"}
        ]
    })";
    FakeTransport fake;
    fake.responseBody = OpenAiResponse(analysis);

    RemoteVlmEngine engine(&fake);
    EngineConfig cfg = MakeRemoteConfig();
    cfg.apiKey = "EMPTY";
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
    EXPECT_EQ(result.at("screenshot").as_string(), preview);

    // The request body must carry the preview as the image_url payload.
    Value body;
    std::string perr;
    ASSERT_TRUE(Value::Parse(fake.lastBody, body, perr));
    const Value &content = body.at("messages").items()[1].at("content");
    ASSERT_TRUE(content.is_array());
    EXPECT_EQ(content.items()[1].at("image_url").at("url").as_string(), preview);

    Value status = analyzer.Status();
    EXPECT_EQ(status.at("backend").as_string(), std::string("remote"));
    EXPECT_EQ(status.at("model").as_string(), std::string("qwen2-vl"));
    EXPECT_TRUE(status.at("model_ready").as_bool());
}

}  // namespace
}  // namespace ScreenParser
}  // namespace OHOS
