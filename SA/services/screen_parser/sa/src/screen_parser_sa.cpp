/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_parser_sa.h"

#include "core_vision_ocr_engine.h"
#include "http_debug_server.h"
#include "i_vlm_engine.h"
#include "ms_lite_engine.h"
#include "remote_vlm_engine.h"
#include "screen_error.h"

namespace OHOS {
namespace ScreenParser {

namespace {
// Optional JSON config; when absent the service falls back to built-in defaults
// (on-device MindSpore Lite backend).
constexpr const char *kConfigPath = "/system/etc/screenparser/service_config.json";

std::string ErrorJson(ScreenError code, const std::string &message) {
    json::Value obj = json::Value::MakeObject();
    obj.set("error", json::Value(message));
    obj.set("code", json::Value(ScreenErrorName(code)));
    return obj.dump();
}
}  // namespace

REGISTER_SYSTEM_ABILITY_BY_ID(ScreenParserSA, kScreenParserSaId, true)

ScreenParserSA::ScreenParserSA(int32_t saId, bool runOnCreate)
    : SystemAbility(saId, runOnCreate) {}

ScreenParserSA::~ScreenParserSA() {
    // Ensure no background thread outlives the SA members it references.
    StopBackgroundWork();
}

ServiceConfig ScreenParserSA::LoadConfig() {
    bool ok = false;
    // LoadFromFile returns built-in defaults when the file is missing/invalid,
    // so the service always starts with a usable configuration.
    return ServiceConfig::LoadFromFile(kConfigPath, ok);
}

ScreenAnalyzerConfig ScreenParserSA::BuildAnalyzerConfig(const ServiceConfig &svc) {
    ScreenAnalyzerConfig config;
    config.pendingThreshold = svc.pendingThreshold;
    config.maxNodes = static_cast<size_t>(svc.maxNodes);
    config.enableOcr = svc.enableOcr;
    config.enableNodeTree = svc.enableNodeTree;

    EngineConfig &engine = config.engine;
    engine.backend = svc.IsRemote() ? VlmBackend::kRemote : VlmBackend::kMsLite;

    // On-device (MindSpore Lite) assets.
    engine.visionModelPath = svc.visionModelPath;
    engine.languageModelPath = svc.languageModelPath;
    engine.vocabPath = svc.vocabPath;
    engine.mergesPath = svc.mergesPath;
    engine.imageInputSize = svc.imageInputSize;
    engine.threadNum = svc.threadNum;
    engine.maxTokens = svc.maxTokens;
    engine.temperature = svc.temperature;

    // Remote (OpenAI-compatible) assets.
    engine.baseUrl = svc.baseUrl;
    engine.model = svc.model;
    engine.apiKey = svc.apiKey;
    engine.timeoutMs = svc.timeoutMs;
    return config;
}

void ScreenParserSA::OnStart() {
    if (started_.exchange(true)) {
        return;
    }
    stopRequested_.store(false);  // clear any stop signal left by a previous run
    ServiceConfig svc = LoadConfig();
    config_ = BuildAnalyzerConfig(svc);

    // Select the VLM backend: on-device MindSpore Lite (default) or a remote
    // OpenAI-compatible endpoint. OCR is always on-device and independent.
    if (svc.IsRemote()) {
        engine_ = std::make_unique<RemoteVlmEngine>();
    } else {
        engine_ = std::make_unique<MSLiteEngine>();
    }
    ocrEngine_ = std::make_unique<CoreVisionOcrEngine>();
    analyzer_ = std::make_unique<ScreenAnalyzer>(engine_.get(), config_, ocrEngine_.get());

    Publish(this);
    InitEngineAsync();

#if defined(SCREENPARSER_ENABLE_HTTP_DEBUG)
    if (svc.http.enable) {
        httpServer_ = svc.http.staticDir.empty()
                          ? std::make_unique<HttpDebugServer>(this)
                          : std::make_unique<HttpDebugServer>(this, svc.http.staticDir);
        httpServer_->Start(svc.http.host, svc.http.port);
    }
#endif
}

void ScreenParserSA::OnStop() {
    // 1) Signal + wait for all background threads (engine loader and in-flight
    //    async analyses) so nothing touches SA members once we release them.
    StopBackgroundWork();

    // 2) Stop the HTTP debug server (joins its handler threads).
    if (httpServer_) {
        httpServer_->Stop();
        httpServer_.reset();
    }

    // 3) Release the pipeline objects. analyzer_ holds raw pointers into
    //    engine_/ocrEngine_, so drop it first. Guarded by mutex_ to stay
    //    consistent with the synchronous IPC handlers.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        analyzer_.reset();
        engine_.reset();
        ocrEngine_.reset();
    }

    engineReady_.store(false);
    started_.store(false);
}

void ScreenParserSA::StopBackgroundWork() {
    // Ask the loader to bail at its next checkpoint, then wait for it. If a model
    // import is already inside IVlmEngine::Load() it cannot be interrupted, so
    // this blocks until that call returns (correct behaviour, and no UAF).
    stopRequested_.store(true);
    if (loaderThread_.joinable()) {
        loaderThread_.join();
    }
    // Wait for every dispatched async analysis to finish before members go away.
    std::unique_lock<std::mutex> lock(asyncMutex_);
    asyncCv_.wait(lock, [this]() { return asyncRunning_ == 0; });
}

void ScreenParserSA::InitEngineAsync() {
    // Model import is slow; run it on a joinable member thread (NOT detached) so
    // OnStop()/dtor can wait for it. This closes the use-after-free window where
    // a detached loader kept dereferencing engine_/ocrEngine_ after teardown.
    if (loaderThread_.joinable()) {
        loaderThread_.join();  // defensive: reap a previous run before reassigning
    }
    loaderThread_ = std::thread([this]() {
        // Acquire the OCR backend first (best-effort). OCR availability is
        // independent of the VLM: RecognizeText works even if the VLM is still
        // loading or fails, so an OCR failure must never block model load.
        std::string ocrError;
        if (ocrEngine_ && !stopRequested_.load()) {
            ocrEngine_->Init(ocrError);
        }
        if (stopRequested_.load() || engine_ == nullptr) {
            return;
        }
        std::string error;
        bool ok = engine_->Load(config_.engine, error);
        // Only publish readiness if a stop was not requested mid-load.
        if (ok && !stopRequested_.load()) {
            engineReady_.store(true);
        }
    });
}

std::string ScreenParserSA::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!analyzer_) {
        return ErrorJson(ScreenError::kModelNotReady, "service not started");
    }
    return analyzer_->Status().dump();
}

std::string ScreenParserSA::AnalyzeSync(int32_t timeoutMs) {
    (void)timeoutMs;  // enforced by the IPC caller side; inference is synchronous here
    std::lock_guard<std::mutex> lock(mutex_);
    if (!analyzer_ || !engineReady_.load()) {
        return ErrorJson(ScreenError::kModelNotReady, "视觉模型尚未就绪");
    }
    try {
        return analyzer_->Analyze().dump();
    } catch (const ScreenException &e) {
        return ErrorJson(e.code(), e.what());
    } catch (const std::exception &e) {
        return ErrorJson(ScreenError::kInferenceFailed, e.what());
    }
}

int32_t ScreenParserSA::AnalyzeAsync(const sptr<IScreenParserCallback> &callback) {
    if (callback == nullptr) {
        return static_cast<int32_t>(IScreenParser::Error::kInvalidArgument);
    }
    // Register the in-flight task under asyncMutex_ and refuse new work once a
    // stop has been requested, so StopBackgroundWork() can wait for the count to
    // reach zero and guarantee no task outlives the SA members.
    {
        std::lock_guard<std::mutex> lock(asyncMutex_);
        if (stopRequested_.load()) {
            return static_cast<int32_t>(IScreenParser::Error::kInvalidArgument);
        }
        ++asyncRunning_;
    }
    // The worker still detaches, but its lifetime is bounded by asyncRunning_:
    // OnStop()/dtor block until it decrements, so `this` stays valid throughout.
    std::thread([this, callback]() {
        RunAnalyzeAsync(callback);
        // Decrement + notify under the lock to avoid a teardown race where
        // StopBackgroundWork() could destroy asyncMutex_/asyncCv_ mid-notify.
        std::lock_guard<std::mutex> lock(asyncMutex_);
        --asyncRunning_;
        asyncCv_.notify_all();
    }).detach();
    return static_cast<int32_t>(IScreenParser::Error::kOk);
}

void ScreenParserSA::RunAnalyzeAsync(const sptr<IScreenParserCallback> &callback) {
    std::string result;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!analyzer_ || !engineReady_.load()) {
            callback->OnError(static_cast<int32_t>(ScreenError::kModelNotReady), "视觉模型尚未就绪");
            return;
        }
        result = analyzer_->Analyze().dump();
    } catch (const ScreenException &e) {
        callback->OnError(static_cast<int32_t>(e.code()), e.what());
        return;
    } catch (const std::exception &e) {
        callback->OnError(static_cast<int32_t>(ScreenError::kInferenceFailed), e.what());
        return;
    }
    callback->OnResult(result);
}

std::string ScreenParserSA::RecognizeText(int32_t timeoutMs) {
    (void)timeoutMs;  // OCR is synchronous here; timeout is enforced caller-side.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!analyzer_) {
        return ErrorJson(ScreenError::kModelNotReady, "service not started");
    }
    // RecognizeText depends only on the OCR backend, not on the VLM, so it is
    // served regardless of engineReady_.
    try {
        return analyzer_->RecognizeText().dump();
    } catch (const ScreenException &e) {
        return ErrorJson(e.code(), e.what());
    } catch (const std::exception &e) {
        return ErrorJson(ScreenError::kInferenceFailed, e.what());
    }
}

}  // namespace ScreenParser
}  // namespace OHOS
