/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Remote vision-language engine backed by an OpenAI-compatible Chat
// Completions endpoint - the same contract the original Python prototype used
// (SCREEN_VLM_BASE_URL + /chat/completions, bearer auth, image_url content).
//
// Request/response building and parsing are pure logic and unit-testable. The
// network exchange is delegated to an injected IHttpTransport; when none is
// supplied the engine creates the build-default transport on Load() (socket on
// device, stub on host), mirroring how the OCR/MSLite backends are isolated.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_REMOTE_VLM_ENGINE_H
#define FOUNDATION_SCREENPARSER_INFERENCE_REMOTE_VLM_ENGINE_H

#include <memory>
#include <string>

#include "i_http_transport.h"
#include "i_vlm_engine.h"
#include "json.h"

namespace OHOS {
namespace ScreenParser {

class RemoteVlmEngine : public IVlmEngine {
public:
    // transport is borrowed and may be null (a default is created on Load).
    explicit RemoteVlmEngine(IHttpTransport *transport = nullptr);
    ~RemoteVlmEngine() override;

    bool Load(const EngineConfig &config, std::string &error) override;
    bool IsReady() const override { return ready_; }
    bool Generate(const GenerateRequest &request, std::string &outText, std::string &error) override;

    // Replace the transport (tests inject a fake). Takes effect on next call.
    void SetTransport(IHttpTransport *transport) { transport_ = transport; }

    // ---- Pure logic, exposed for unit tests -------------------------------

    // Resolve the absolute /chat/completions URL from a base URL (trailing
    // slashes tolerated; an explicit /chat/completions suffix is preserved).
    static std::string CompletionsUrl(const std::string &baseUrl);

    // Build the OpenAI chat-completions JSON body (system + user text/image).
    static std::string BuildRequestBody(const EngineConfig &config, const GenerateRequest &request);

    // Extract choices[0].message.content from a response body. Returns false
    // (error set) for an error object, missing choices, or empty content.
    static bool ExtractAssistantText(const std::string &responseBody, std::string &outText,
                                     std::string &error);

private:
    EngineConfig config_;
    IHttpTransport *transport_ = nullptr;             // borrowed
    std::unique_ptr<IHttpTransport> ownedTransport_;  // created when none injected
    bool ready_ = false;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_REMOTE_VLM_ENGINE_H
