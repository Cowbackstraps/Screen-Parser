/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// MindSpore Lite backed vision-language engine (on-device inference).
//
// Owns the lite sessions exported from Qwen2-VL:
//   * vision encoder : pixel_values -> image embeddings [numImageTokens, hidden]
//   * language model : (inputs_embeds, position_ids, past_kv) -> logits, present_kv
// and a QwenTokenizer plus the token-embedding table.
//
// Multimodal fusion uses the inputs_embeds contract (the portable, standard
// approach for on-device VLMs): text tokens are looked up in the embedding
// table, and the image placeholder token is replaced by the vision embeddings,
// so the screenshot is genuinely part of the language-model prompt. Generation
// is greedy autoregressive decoding; the JsonConstraintDecoder decides when the
// emitted JSON object is complete so the loop can stop early.
//
// This translation unit only compiles when SCREENPARSER_ENABLE_MSLITE is set
// (i.e. inside the OpenHarmony tree with //foundation/ai/mindspore_lite). Host
// unit tests use the fake engine instead.

#ifndef FOUNDATION_SCREENPARSER_INFERENCE_MS_LITE_ENGINE_H
#define FOUNDATION_SCREENPARSER_INFERENCE_MS_LITE_ENGINE_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "i_vlm_engine.h"
#include "qwen_tokenizer.h"

namespace OHOS {
namespace ScreenParser {

class MSLiteEngine : public IVlmEngine {
public:
    // Constructor/destructor are defined out-of-line in the .cpp (where Impl is
    // complete). An inline `= default` ctor here would make every make_unique
    // call site instantiate the unique_ptr<Impl> cleanup path and fail with
    // "sizeof incomplete type Impl" (classic pimpl pitfall).
    MSLiteEngine();
    ~MSLiteEngine() override;

    bool Load(const EngineConfig &config, std::string &error) override;
    bool IsReady() const override { return ready_; }
    bool Generate(const GenerateRequest &request, std::string &outText, std::string &error) override;

    // ---- Pure, backend-independent helpers (always compiled; host-tested) ---
    // These carry the tricky multimodal math so it is verifiable off-device,
    // independent of MindSpore Lite. The on-device glue only memcpys buffers.

    // Look up token embeddings from a row-major [vocab, hidden] table. Out-of-
    // range ids map to a zero vector. Returns [ids.size() * hidden] row-major.
    static std::vector<float> EmbedTokens(const std::vector<int32_t> &ids,
                                          const std::vector<float> &table, int32_t hidden);
    // Assemble the prompt as an inputs_embeds matrix [seq, hidden]: every id
    // equal to imageTokenId is expanded to the whole imageEmbeds block
    // ([numImageTokens, hidden]); other ids use the embedding table. This is how
    // the screenshot actually reaches the language model.
    static std::vector<float> BuildPromptEmbeds(const std::vector<int32_t> &ids,
                                                const std::vector<float> &table, int32_t hidden,
                                                const std::vector<float> &imageEmbeds,
                                                int32_t imageTokenId);
    static int32_t ArgMax(const float *logits, int32_t size);

private:
    // Run the vision encoder, returning flattened image embeddings.
    bool EncodeImage(const ImageTensor &image, std::vector<float> &embeddings, std::string &error);
    // Single language-model forward over inputs_embeds ([seq, hidden], seq >= 1)
    // starting at absolute `position`, threading the flat KV cache in/out.
    // Returns the greedy next token id from the LAST sequence position.
    bool ForwardStep(const std::vector<float> &inputsEmbeds, int32_t position,
                     std::vector<float> &kvCache, int32_t &nextToken, std::string &error);

    EngineConfig config_;
    std::unique_ptr<QwenTokenizer> tokenizer_;
    bool ready_ = false;
    // Serialize Generate(): MindSpore Lite sessions are not thread-safe, and
    // AnalyzeAsync runs inference on a detached worker thread.
    std::mutex generateMutex_;

    // Opaque handles to avoid leaking MindSpore headers into includers.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_INFERENCE_MS_LITE_ENGINE_H
