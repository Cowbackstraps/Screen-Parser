/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// ScreenParserSA: the System Ability that hosts the screen parsing service.
// Registers with SAMGR under kScreenParserSaId, loads the on-device VLM
// asynchronously (so SA startup is not blocked by model import), and serves the
// IPC contract. An optional HTTP debug server can be enabled to reuse the
// existing web front-end.

#ifndef FOUNDATION_SCREENPARSER_SA_SCREEN_PARSER_SA_H
#define FOUNDATION_SCREENPARSER_SA_SCREEN_PARSER_SA_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "i_screen_parser.h"
#include "screen_analyzer.h"
#include "screen_parser_stub.h"
#include "service_config.h"
#include "system_ability.h"

namespace OHOS {
namespace ScreenParser {

class HttpDebugServer;
class IVlmEngine;
class IOcrEngine;

class ScreenParserSA : public SystemAbility, public ScreenParserStub {
public:
    DECLARE_SYSTEM_ABILITY(ScreenParserSA);

    ScreenParserSA(int32_t saId, bool runOnCreate);
    ~ScreenParserSA() override;

    // Lifecycle overrides. Exposed publicly (base SystemAbility declares them
    // protected) so the standalone host process in main.cpp can drive startup:
    // OnStart() loads config, builds the engines, Publish()es to samgr and
    // starts the optional HTTP debug server.
    void OnStart() override;
    void OnStop() override;

    // IScreenParser implementation.
    std::string GetStatus() override;
    std::string AnalyzeSync(int32_t timeoutMs) override;
    int32_t AnalyzeAsync(const sptr<IScreenParserCallback> &callback) override;
    std::string RecognizeText(int32_t timeoutMs) override;

private:
    // Loads the engine off the main thread, then marks the service ready.
    void InitEngineAsync();
    void RunAnalyzeAsync(const sptr<IScreenParserCallback> &callback);
    // Signals background threads to stop and waits for them to finish, so no
    // thread touches SA members after teardown begins (prevents use-after-free).
    void StopBackgroundWork();

    // Load the service configuration (config file, falling back to defaults).
    static ServiceConfig LoadConfig();
    // Map a ServiceConfig onto the analyzer config (engine + pipeline).
    static ScreenAnalyzerConfig BuildAnalyzerConfig(const ServiceConfig &svc);

    std::mutex mutex_;
    std::unique_ptr<IVlmEngine> engine_;
    std::unique_ptr<IOcrEngine> ocrEngine_;
    std::unique_ptr<ScreenAnalyzer> analyzer_;
    std::unique_ptr<HttpDebugServer> httpServer_;
    std::atomic<bool> engineReady_{false};
    std::atomic<bool> started_{false};
    ScreenAnalyzerConfig config_;

    // Background-work lifetime control. The engine loader runs on a joinable
    // member thread; async analyses are counted so OnStop()/dtor can wait for
    // all in-flight work before members are released (no detached-thread UAF).
    std::thread loaderThread_;
    std::atomic<bool> stopRequested_{false};
    std::mutex asyncMutex_;
    std::condition_variable asyncCv_;
    int asyncRunning_{0};
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_SA_SCREEN_PARSER_SA_H
