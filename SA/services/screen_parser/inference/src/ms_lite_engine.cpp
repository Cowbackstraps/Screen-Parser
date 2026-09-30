/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "ms_lite_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "json.h"
#include "json_constraint_decoder.h"
#include "screen_error.h"

#if defined(SCREENPARSER_ENABLE_MSLITE)
#include "include/api/model.h"
#include "include/api/context.h"
#include "include/api/status.h"
#include "include/api/types.h"
#endif

namespace OHOS {
namespace ScreenParser {

// ---------------------------------------------------------------------------
// Pure, backend-independent logic (ALWAYS compiled).
//
// The multimodal math lives here so it is verifiable on the host without
// MindSpore Lite: token-embedding lookup, splicing the vision embeddings into
// the prompt, and greedy argmax. The on-device glue below only memcpys buffers
// into lite tensors and calls these helpers, so a bug in the fusion logic is
// caught by host unit tests (test_mslite_logic.cpp) rather than on a device.
// ---------------------------------------------------------------------------

std::vector<float> MSLiteEngine::EmbedTokens(const std::vector<int32_t> &ids,
                                             const std::vector<float> &table, int32_t hidden) {
    std::vector<float> out;
    if (hidden <= 0) {
        return out;
    }
    const int32_t vocab = static_cast<int32_t>(table.size() / static_cast<size_t>(hidden));
    out.reserve(ids.size() * static_cast<size_t>(hidden));
    for (int32_t id : ids) {
        if (id >= 0 && id < vocab) {
            const size_t base = static_cast<size_t>(id) * static_cast<size_t>(hidden);
            out.insert(out.end(), table.begin() + base, table.begin() + base + hidden);
        } else {
            // Out-of-vocabulary / placeholder id: emit a zero row so the layout
            // stays [seq, hidden]. Callers splice real embeddings over these.
            out.insert(out.end(), static_cast<size_t>(hidden), 0.0f);
        }
    }
    return out;
}

std::vector<float> MSLiteEngine::BuildPromptEmbeds(const std::vector<int32_t> &ids,
                                                   const std::vector<float> &table, int32_t hidden,
                                                   const std::vector<float> &imageEmbeds,
                                                   int32_t imageTokenId) {
    std::vector<float> out;
    if (hidden <= 0) {
        return out;
    }
    const int32_t vocab = static_cast<int32_t>(table.size() / static_cast<size_t>(hidden));
    const bool imageUsable =
        !imageEmbeds.empty() && imageEmbeds.size() % static_cast<size_t>(hidden) == 0;
    for (int32_t id : ids) {
        if (id == imageTokenId && imageUsable) {
            // Expand the single placeholder into the full vision embedding block
            // ([numImageTokens, hidden]). This is how the screenshot is fed to
            // the language model.
            out.insert(out.end(), imageEmbeds.begin(), imageEmbeds.end());
        } else if (id >= 0 && id < vocab) {
            const size_t base = static_cast<size_t>(id) * static_cast<size_t>(hidden);
            out.insert(out.end(), table.begin() + base, table.begin() + base + hidden);
        } else {
            out.insert(out.end(), static_cast<size_t>(hidden), 0.0f);
        }
    }
    return out;
}

int32_t MSLiteEngine::ArgMax(const float *logits, int32_t size) {
    int32_t best = 0;
    if (logits == nullptr || size <= 0) {
        return 0;
    }
    float bestValue = logits[0];
    for (int32_t i = 1; i < size; ++i) {
        if (logits[i] > bestValue) {
            bestValue = logits[i];
            best = i;
        }
    }
    return best;
}

#if defined(SCREENPARSER_ENABLE_MSLITE)

// ---------------------------------------------------------------------------
// MindSpore Lite implementation (compiled only inside the OpenHarmony tree).
// ---------------------------------------------------------------------------

namespace {

// Directory portion of a path (supports both '/' and '\\' separators).
std::string DirName(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool ReadTextFile(const std::string &path, std::string &out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// Read a raw little-endian float32 blob (the exported token-embedding matrix).
bool ReadBinaryFloats(const std::string &path, std::vector<float> &out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    std::streamsize bytes = file.tellg();
    if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(float)) != 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(bytes / sizeof(float)));
    if (!file.read(reinterpret_cast<char *>(out.data()), bytes)) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

struct MSLiteEngine::Impl {
    std::shared_ptr<mindspore::Context> context;
    std::unique_ptr<mindspore::lite::MindSporeSession> visionSession;
    std::unique_ptr<mindspore::lite::MindSporeSession> languageSession;
    std::vector<char> visionBuffer;
    std::vector<char> languageBuffer;
    // Token-embedding table ([vocab, hidden] row-major) plus model metadata used
    // to fuse the vision embeddings into the language-model prompt.
    std::vector<float> embedTable;
    int32_t hiddenSize = 0;
    int32_t vocabSize = 0;
    int32_t imageTokenId = 151655;  // Qwen2-VL <|image_pad|> default.
};

MSLiteEngine::MSLiteEngine() = default;
MSLiteEngine::~MSLiteEngine() = default;

bool MSLiteEngine::Load(const EngineConfig &config, std::string &error) {
    config_ = config;
    if (config.visionModelPath.empty() || config.languageModelPath.empty()) {
        error = "模型路径未配置（vision/language .ms）";
        return false;
    }
    if (config.vocabPath.empty() || config.mergesPath.empty()) {
        error = "分词器资源未配置（vocab.json/merges.txt）";
        return false;
    }
    impl_ = std::make_unique<Impl>();

    tokenizer_ = std::make_unique<QwenTokenizer>();
    if (!tokenizer_->Load(config.vocabPath, config.mergesPath, error)) {
        return false;
    }
    // Qwen2 special tokens (overridable via asset metadata in a full port).
    tokenizer_->SetSpecialTokens(/*bos=*/-1, /*eos=*/151643, /*pad=*/151643);

    impl_->context = std::make_shared<mindspore::Context>();
    impl_->context->mutable_config().mode = mindspore::kMindIR;
    auto &cpu = impl_->context->mutable_config().device_configs.emplace_back();
    cpu.device_cfg.mutable_type() = mindspore::DeviceType::kCPU;
    cpu.thread_num = config.threadNum;

    auto loadSession = [&](const std::string &path, std::vector<char> &buffer,
                           std::unique_ptr<mindspore::lite::MindSporeSession> &session) -> bool {
        buffer = mindspore::lite::Model::ReadFile(path);
        if (buffer.empty()) {
            error = "cannot read model file: " + path;
            return false;
        }
        session = std::make_unique<mindspore::lite::MindSporeSession>();
        if (session->CompileGraph(buffer.data(), buffer.size(), impl_->context) !=
            mindspore::kSuccess) {
            error = "failed to compile model: " + path;
            return false;
        }
        return true;
    };

    if (!loadSession(config.visionModelPath, impl_->visionBuffer, impl_->visionSession) ||
        !loadSession(config.languageModelPath, impl_->languageBuffer, impl_->languageSession)) {
        return false;
    }

    // Load the token-embedding matrix and metadata that sit next to the language
    // model (both produced by models/export_qwen2vl_onnx.py). The inputs_embeds
    // fusion needs the table to embed text tokens on the CPU before splicing the
    // vision embeddings in.
    const std::string dir = DirName(config.languageModelPath);
    if (!ReadBinaryFloats(dir + "/qwen2vl_embed.bin", impl_->embedTable)) {
        error = "无法读取词嵌入矩阵: " + dir + "/qwen2vl_embed.bin";
        return false;
    }
    int64_t hidden = 0;
    int64_t vocab = 0;
    int64_t imageToken = 151655;
    std::string metaText;
    if (ReadTextFile(dir + "/model_meta.json", metaText)) {
        json::Value meta;
        std::string jsonError;
        if (json::Value::Parse(metaText, meta, jsonError) && meta.is_object()) {
            hidden = meta.at("hidden_size").as_int(0);
            vocab = meta.at("vocab_size").as_int(0);
            imageToken = meta.at("image_token_id").as_int(151655);
        }
    }
    if (vocab <= 0) {
        error = "model_meta.json 缺失或无 vocab_size（应与语言模型同目录）";
        return false;
    }
    if (hidden <= 0) {
        hidden = static_cast<int64_t>(impl_->embedTable.size()) / vocab;
    }
    if (hidden <= 0 ||
        impl_->embedTable.size() != static_cast<size_t>(vocab) * static_cast<size_t>(hidden)) {
        error = "词嵌入矩阵尺寸与 meta 不一致（期望 vocab_size*hidden_size 个 float）";
        return false;
    }
    impl_->hiddenSize = static_cast<int32_t>(hidden);
    impl_->vocabSize = static_cast<int32_t>(vocab);
    impl_->imageTokenId = static_cast<int32_t>(imageToken);

    ready_ = true;
    return true;
}

bool MSLiteEngine::EncodeImage(const ImageTensor &image, std::vector<float> &embeddings,
                               std::string &error) {
    if (image.Empty()) {
        error = "empty image tensor";
        return false;
    }
    if (impl_ == nullptr || impl_->visionSession == nullptr) {
        error = "vision session not initialized";
        return false;
    }
    auto inputs = impl_->visionSession->GetInputs();
    if (inputs.empty()) {
        error = "vision model has no inputs";
        return false;
    }
    // Bind the CHW float pixels to the first input tensor.
    auto *inputTensor = reinterpret_cast<mindspore::MSTensor *>(inputs.front());
    size_t need = image.data.size() * sizeof(float);
    if (inputTensor->DataSize() < need) {
        error = "vision input tensor too small";
        return false;
    }
    std::memcpy(inputTensor->MutableData(), image.data.data(), need);

    std::vector<mindspore::MSTensor> outputs;
    if (impl_->visionSession->RunGraph(inputs, &outputs) != mindspore::kSuccess || outputs.empty()) {
        error = "vision encoder inference failed";
        return false;
    }
    const float *data = reinterpret_cast<const float *>(outputs.front().Data());
    size_t count = outputs.front().DataSize() / sizeof(float);
    embeddings.assign(data, data + count);
    return true;
}

bool MSLiteEngine::ForwardStep(const std::vector<float> &inputsEmbeds, int32_t position,
                               std::vector<float> &kvCache, int32_t &nextToken, std::string &error) {
    if (impl_ == nullptr || impl_->languageSession == nullptr) {
        error = "language session not initialized";
        return false;
    }
    const int32_t hidden = impl_->hiddenSize;
    if (hidden <= 0 || inputsEmbeds.empty() ||
        inputsEmbeds.size() % static_cast<size_t>(hidden) != 0) {
        error = "inputs_embeds 长度不是 hidden_size 的整数倍";
        return false;
    }
    const int32_t seq = static_cast<int32_t>(inputsEmbeds.size() / static_cast<size_t>(hidden));

    auto inputs = impl_->languageSession->GetInputs();
    if (inputs.size() < 2) {
        error = "language model expects (inputs_embeds, position_ids, past_kv) inputs";
        return false;
    }

    // inputs[0]: inputs_embeds [1, seq, hidden] (float32).
    auto *embedTensor = reinterpret_cast<mindspore::MSTensor *>(inputs[0]);
    const size_t embedBytes = inputsEmbeds.size() * sizeof(float);
    if (embedTensor->DataSize() < embedBytes) {
        error = "language inputs_embeds tensor too small";
        return false;
    }
    std::memcpy(embedTensor->MutableData(), inputsEmbeds.data(), embedBytes);

    // inputs[1]: position_ids [1, seq] int32 = position .. position+seq-1.
    auto *posTensor = reinterpret_cast<mindspore::MSTensor *>(inputs[1]);
    if (posTensor->DataSize() >= static_cast<size_t>(seq) * sizeof(int32_t)) {
        int32_t *posData = reinterpret_cast<int32_t *>(posTensor->MutableData());
        for (int32_t i = 0; i < seq; ++i) {
            posData[i] = position + i;
        }
    }

    // inputs[2]: past KV cache (flat float blob), when present. The exact tensor
    // layout is model-specific; a full port binds per-layer past_key/past_value.
    if (!kvCache.empty() && inputs.size() > 2) {
        auto *kvTensor = reinterpret_cast<mindspore::MSTensor *>(inputs[2]);
        std::memcpy(kvTensor->MutableData(), kvCache.data(),
                    std::min(kvTensor->DataSize(), kvCache.size() * sizeof(float)));
    }

    std::vector<mindspore::MSTensor> outputs;
    if (impl_->languageSession->RunGraph(inputs, &outputs) != mindspore::kSuccess ||
        outputs.empty()) {
        error = "language model inference failed";
        return false;
    }
    // outputs[0] = logits [1, seq, vocab]; sample the LAST position's row.
    const float *logits = reinterpret_cast<const float *>(outputs.front().Data());
    const size_t totalFloats = outputs.front().DataSize() / sizeof(float);
    const int32_t vocab = static_cast<int32_t>(totalFloats / static_cast<size_t>(seq));
    if (vocab <= 0) {
        error = "language model produced malformed logits";
        return false;
    }
    nextToken = ArgMax(logits + static_cast<size_t>(seq - 1) * static_cast<size_t>(vocab), vocab);

    // outputs[1..] = present KV cache; carry it into the next step.
    if (outputs.size() > 1) {
        const float *kv = reinterpret_cast<const float *>(outputs[1].Data());
        size_t count = outputs[1].DataSize() / sizeof(float);
        kvCache.assign(kv, kv + count);
    }
    return true;
}

bool MSLiteEngine::Generate(const GenerateRequest &request, std::string &outText,
                            std::string &error) {
    // Serialize generation: the lite sessions and KV cache are not thread-safe.
    std::lock_guard<std::mutex> lock(generateMutex_);
    if (!ready_ || impl_ == nullptr) {
        error = "engine not ready";
        return false;
    }
    if (impl_->hiddenSize <= 0) {
        error = "embedding metadata not loaded";
        return false;
    }

    // 1. Vision encode (only when a preprocessed image tensor is supplied).
    std::vector<float> imageEmbeds;
    const bool haveImage = request.image != nullptr && !request.image->Empty();
    if (haveImage && !EncodeImage(*request.image, imageEmbeds, error)) {
        return false;
    }

    // 2. Chat-template prompt (Qwen2 ChatML). A single image placeholder token
    //    marks where the vision embeddings are spliced into inputs_embeds.
    const int32_t imageTokenId = impl_->imageTokenId;
    std::vector<int32_t> promptIds =
        tokenizer_->Encode("<|im_start|>system\n" + request.systemPrompt + "<|im_end|>\n" +
                           "<|im_start|>user\n");
    if (haveImage && imageTokenId >= 0) {
        promptIds.push_back(imageTokenId);
    }
    std::vector<int32_t> tailIds =
        tokenizer_->Encode(request.userText + "<|im_end|>\n<|im_start|>assistant\n");
    promptIds.insert(promptIds.end(), tailIds.begin(), tailIds.end());

    // 3. Build inputs_embeds: text tokens via the embedding table, the image
    //    placeholder replaced by the vision embeddings. THIS is where the
    //    screenshot enters the language model (previously the embeddings were
    //    computed and then dropped, so the model never saw the image).
    std::vector<float> promptEmbeds = BuildPromptEmbeds(promptIds, impl_->embedTable,
                                                        impl_->hiddenSize, imageEmbeds,
                                                        imageTokenId);
    if (promptEmbeds.empty()) {
        error = "failed to build prompt embeddings";
        return false;
    }

    const int32_t maxTokens = request.maxTokens > 0 ? request.maxTokens : config_.maxTokens;
    const int32_t timeoutMs = config_.timeoutMs > 0 ? config_.timeoutMs : 60000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    JsonConstraintDecoder constraint;
    constraint.Reset();

    std::vector<int32_t> generated;
    std::vector<float> kvCache;

    // 4. Prefill: one forward over the whole prompt yields the first token.
    int32_t nextToken = 0;
    int32_t position = 0;
    if (!ForwardStep(promptEmbeds, position, kvCache, nextToken, error)) {
        return false;
    }
    position += static_cast<int32_t>(promptEmbeds.size() / static_cast<size_t>(impl_->hiddenSize));

    // 5. Autoregressive decode, one token per step, threading the KV cache.
    for (int32_t step = 0; step < maxTokens; ++step) {
        if (std::chrono::steady_clock::now() > deadline) {
            error = "生成超时（超过 " + std::to_string(timeoutMs) + "ms）";
            return false;
        }
        if (tokenizer_->IsStopToken(nextToken)) {
            break;
        }
        generated.push_back(nextToken);

        std::string piece = tokenizer_->Decode({nextToken});
        constraint.Feed(piece);
        if (constraint.IsComplete() || constraint.IsBroken()) {
            break;
        }

        std::vector<float> stepEmbeds = EmbedTokens({nextToken}, impl_->embedTable,
                                                    impl_->hiddenSize);
        if (!ForwardStep(stepEmbeds, position, kvCache, nextToken, error)) {
            return false;
        }
        position += 1;
    }

    outText = tokenizer_->Decode(generated);
    if (outText.empty()) {
        error = "model produced empty output";
        return false;
    }
    return true;
}

#else  // !SCREENPARSER_ENABLE_MSLITE

// Host / stub build: MindSpore Lite is unavailable, so the engine reports not
// ready. Unit tests use a fake IVlmEngine instead of this backend. The pure
// helpers above are still compiled and host-tested.

struct MSLiteEngine::Impl {};

MSLiteEngine::MSLiteEngine() = default;
MSLiteEngine::~MSLiteEngine() = default;

bool MSLiteEngine::Load(const EngineConfig &config, std::string &error) {
    config_ = config;
    error = "MindSpore Lite backend not enabled in this build";
    return false;
}

bool MSLiteEngine::EncodeImage(const ImageTensor &, std::vector<float> &, std::string &error) {
    error = "MindSpore Lite backend not enabled";
    return false;
}

bool MSLiteEngine::ForwardStep(const std::vector<float> &, int32_t, std::vector<float> &,
                               int32_t &, std::string &error) {
    error = "MindSpore Lite backend not enabled";
    return false;
}

bool MSLiteEngine::Generate(const GenerateRequest &, std::string &, std::string &error) {
    std::lock_guard<std::mutex> lock(generateMutex_);
    error = "MindSpore Lite backend not enabled in this build";
    return false;
}

#endif

}  // namespace ScreenParser
}  // namespace OHOS
