/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Process entry for the screen parser system ability.
//
// This is a standalone SA host process (launched by init via
// etc/init/screenparser.cfg). It waits for samgr, constructs the SA, and
// drives OnStart() directly: OnStart() loads the config, builds the VLM/OCR
// engines, Publish()es the SA to samgr (so clients can look it up by
// kScreenParserSaId) and starts the optional HTTP debug server. This mirrors
// the in-tree storage_daemon standalone SA pattern. The process then blocks on
// the samgr work thread, servicing incoming IPC requests.

#include <new>
#include <unistd.h>

#include "ipc_skeleton.h"
#include "iservice_registry.h"
#include "screen_parser_sa.h"

using namespace OHOS;
using namespace OHOS::ScreenParser;

namespace {
// Interval to re-poll samgr while waiting for it to come up (matches the
// storage_daemon standalone SA reference in-tree).
constexpr useconds_t kSamgrWaitIntervalUs = 3 * 1000;
}  // namespace

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // Wait for samgr so Publish() inside OnStart() can register this SA.
    while (SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager() == nullptr) {
        usleep(kSamgrWaitIntervalUs);
    }
    sptr<ScreenParserSA> sa = new (std::nothrow) ScreenParserSA(kScreenParserSaId, true);
    if (sa == nullptr) {
        return -1;
    }
    // OnStart() is public on ScreenParserSA (base declares it protected); it
    // initializes the pipeline and publishes the SA to samgr.
    sa->OnStart();
    // Block forever, handling incoming IPC requests on the samgr work thread.
    IPCSkeleton::JoinWorkThread();
    return 0;
}
