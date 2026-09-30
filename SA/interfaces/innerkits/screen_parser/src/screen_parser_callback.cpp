/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_parser_callback.h"

#include "ipc_skeleton.h"
#include "message_parcel.h"
#include "message_option.h"

namespace OHOS {
namespace ScreenParser {

int ScreenParserCallbackStub::OnRemoteRequest(uint32_t code, MessageParcel &data,
                                              MessageParcel &reply, MessageOption &option) {
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != IScreenParserCallback::GetDescriptor()) {
        return static_cast<int>(IScreenParser::Error::kInvalidArgument);
    }
    switch (static_cast<IScreenParserCallback::Message>(code)) {
        case IScreenParserCallback::Message::ON_RESULT: {
            std::string result = data.ReadString();
            reply.WriteInt32(OnResult(result));
            return 0;
        }
        case IScreenParserCallback::Message::ON_ERROR: {
            int32_t errCode = data.ReadInt32();
            std::string message = data.ReadString();
            reply.WriteInt32(OnError(errCode, message));
            return 0;
        }
        default:
            return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
    }
}

int32_t ScreenParserCallbackProxy::OnResult(const std::string &resultJson) {
    MessageOption option;
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParserCallback::GetDescriptor());
    data.WriteString(resultJson);
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParserCallback::Message::ON_RESULT), data,
                              reply, option) != 0) {
        return static_cast<int32_t>(IScreenParser::Error::kIpcFailure);
    }
    return reply.ReadInt32();
}

int32_t ScreenParserCallbackProxy::OnError(int32_t code, const std::string &message) {
    MessageOption option;
    MessageParcel data;
    MessageParcel reply;
    data.WriteInterfaceToken(IScreenParserCallback::GetDescriptor());
    data.WriteInt32(code);
    data.WriteString(message);
    if (Remote()->SendRequest(static_cast<uint32_t>(IScreenParserCallback::Message::ON_ERROR), data,
                              reply, option) != 0) {
        return static_cast<int32_t>(IScreenParser::Error::kIpcFailure);
    }
    return reply.ReadInt32();
}

}  // namespace ScreenParser
}  // namespace OHOS
