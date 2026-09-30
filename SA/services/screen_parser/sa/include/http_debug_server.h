/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Optional local HTTP debug server.
//
// Reuses the exact routes of the Python prototype (server.py) so the existing
// web front-end in services/screen_parser/static works unchanged:
//   GET  /api/status   -> IScreenParser::GetStatus()
//   POST /api/analyze  -> IScreenParser::AnalyzeSync()
//   POST /api/ocr      -> IScreenParser::RecognizeText()
//   GET  /<file>       -> static asset
//
// Compiled only when SCREENPARSER_ENABLE_HTTP_DEBUG is defined. It is a debug /
// bring-up convenience; production clients use the IPC interface.

#ifndef FOUNDATION_SCREENPARSER_SA_HTTP_DEBUG_SERVER_H
#define FOUNDATION_SCREENPARSER_SA_HTTP_DEBUG_SERVER_H

#include <atomic>
#include <string>
#include <thread>

#include "i_screen_parser.h"

namespace OHOS {
namespace ScreenParser {

class HttpDebugServer {
public:
    // service is borrowed and must outlive the server.
    explicit HttpDebugServer(IScreenParser *service,
                             std::string staticDir = "/system/etc/screenparser/static");
    ~HttpDebugServer();

    bool Start(const std::string &host, int port);
    void Stop();

private:
    void AcceptLoop();
    void HandleClient(int clientFd);

    IScreenParser *service_;
    std::string staticDir_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int listenFd_ = -1;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_SA_HTTP_DEBUG_SERVER_H
