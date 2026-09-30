/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_parser_proxy.h"

#include <sys/mman.h>
#include <unistd.h>

#include "message_parcel.h"
#include "message_option.h"

namespace OHOS {
namespace ScreenParser {

std::string ScreenParserProxy::ReadLargeString(MessageParcel &reply) {
    bool usedAshmem = reply.ReadBool();
    if (!usedAshmem) {
        return reply.ReadString();
    }
    int32_t size = reply.ReadInt32();
    int fd = reply.ReadFileDescriptor();
    if (size <= 0 || fd < 0) {
        return std::string();
    }
    // This OHOS Ashmem API has no FromFd(), so map the received fd directly.
    // The fd refers to the same shared region the stub wrote into.
    void *addr = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        return std::string();
    }
    std::string result(static_cast<const char *>(addr), static_cast<size_t>(size));
    munmap(addr, static_cast<size_t>(size));
    return result;
}

std::string ScreenParserProxy::GetStatus() {
    MessageOption option;
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParser::GetDescriptor());
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParser::Message::GET_STATUS), data,
                              reply, option) != 0) {
        return std::string();
    }
    return ReadLargeString(reply);
}

std::string ScreenParserProxy::AnalyzeSync(int32_t timeoutMs) {
    MessageOption option;
    // Analysis is slow (on-device inference); allow a blocking async-style call.
    option.SetFlags(MessageOption::TF_SYNC);
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParser::GetDescriptor());
    data.WriteInt32(timeoutMs);
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParser::Message::ANALYZE_SYNC), data,
                              reply, option) != 0) {
        return std::string();
    }
    return ReadLargeString(reply);
}

std::string ScreenParserProxy::RecognizeText(int32_t timeoutMs) {
    MessageOption option;
    option.SetFlags(MessageOption::TF_SYNC);
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParser::GetDescriptor());
    data.WriteInt32(timeoutMs);
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParser::Message::RECOGNIZE_TEXT), data,
                              reply, option) != 0) {
        return std::string();
    }
    return ReadLargeString(reply);
}

int32_t ScreenParserProxy::AnalyzeAsync(const sptr<IScreenParserCallback> &callback) {
    if (callback == nullptr) {
        return static_cast<int32_t>(IScreenParser::Error::kInvalidArgument);
    }
    MessageOption option;
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParser::GetDescriptor());
    sptr<IRemoteObject> remote = callback->AsObject();
    if (remote == nullptr) {
        return static_cast<int32_t>(IScreenParser::Error::kInvalidArgument);
    }
    data.WriteRemoteObject(remote);
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParser::Message::ANALYZE_ASYNC), data,
                              reply, option) != 0) {
        return static_cast<int32_t>(IScreenParser::Error::kIpcFailure);
    }
    return reply.ReadInt32();
}

}  // namespace ScreenParser
}  // namespace OHOS
