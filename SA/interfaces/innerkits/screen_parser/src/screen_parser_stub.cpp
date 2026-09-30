/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_parser_stub.h"

#include "ashmem.h"
#include "ipc_skeleton.h"
#include "message_parcel.h"

namespace OHOS {
namespace ScreenParser {

namespace {
// Payloads above this size are shipped through ashmem instead of the parcel
// buffer to avoid exceeding the IPC transaction limit (screenshot base64 can
// be several MB).
constexpr size_t kAshmemThreshold = 512 * 1024;

void WriteLargeString(MessageParcel &reply, const std::string &value) {
    if (value.size() <= kAshmemThreshold) {
        reply.WriteBool(false);
        reply.WriteString(value);
        return;
    }
    sptr<Ashmem> ashmem = Ashmem::CreateAshmem("screenparser_result", static_cast<int32_t>(value.size()));
    // This OHOS Ashmem API has no MapWriteOnlyAshmem(); map read/write before writing.
    if (ashmem == nullptr || !ashmem->MapReadAndWriteAshmem()) {
        // Fall back to inline write; may fail for very large payloads.
        reply.WriteBool(false);
        reply.WriteString(value);
        return;
    }
    ashmem->WriteToAshmem(value.data(), static_cast<int32_t>(value.size()), 0);
    reply.WriteBool(true);
    reply.WriteInt32(static_cast<int32_t>(value.size()));
    reply.WriteFileDescriptor(ashmem->GetAshmemFd());
}
}  // namespace

int ScreenParserStub::OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                                      MessageOption &option) {
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != IScreenParser::GetDescriptor()) {
        return static_cast<int>(IScreenParser::Error::kInvalidArgument);
    }

    switch (static_cast<IScreenParser::Message>(code)) {
        case IScreenParser::Message::GET_STATUS:
            HandleGetStatus(data, reply);
            return 0;
        case IScreenParser::Message::ANALYZE_SYNC:
            HandleAnalyzeSync(data, reply);
            return 0;
        case IScreenParser::Message::ANALYZE_ASYNC:
            HandleAnalyzeAsync(data, reply);
            return 0;
        case IScreenParser::Message::RECOGNIZE_TEXT:
            HandleRecognizeText(data, reply);
            return 0;
        default:
            return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
    }
}

void ScreenParserStub::HandleGetStatus(MessageParcel &data, MessageParcel &reply) {
    (void)data;
    WriteLargeString(reply, GetStatus());
}

void ScreenParserStub::HandleAnalyzeSync(MessageParcel &data, MessageParcel &reply) {
    int32_t timeoutMs = data.ReadInt32();
    WriteLargeString(reply, AnalyzeSync(timeoutMs));
}

void ScreenParserStub::HandleAnalyzeAsync(MessageParcel &data, MessageParcel &reply) {
    sptr<IRemoteObject> remote = data.ReadRemoteObject();
    if (remote == nullptr) {
        reply.WriteInt32(static_cast<int32_t>(IScreenParser::Error::kInvalidArgument));
        return;
    }
    sptr<IScreenParserCallback> callback = iface_cast<IScreenParserCallback>(remote);
    if (callback == nullptr) {
        reply.WriteInt32(static_cast<int32_t>(IScreenParser::Error::kInvalidArgument));
        return;
    }
    reply.WriteInt32(AnalyzeAsync(callback));
}

void ScreenParserStub::HandleRecognizeText(MessageParcel &data, MessageParcel &reply) {
    int32_t timeoutMs = data.ReadInt32();
    WriteLargeString(reply, RecognizeText(timeoutMs));
}

}  // namespace ScreenParser
}  // namespace OHOS
