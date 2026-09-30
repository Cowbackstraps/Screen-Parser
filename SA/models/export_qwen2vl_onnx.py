#!/usr/bin/env python3
"""Export Qwen2-VL into MindSpore Lite assets matching MSLiteEngine's contract.

Run this on the development machine (needs torch, transformers, onnx). It
produces, into --out:

  qwen2vl_vision.onnx  -- vision encoder : pixel_values -> image_embeds
                          (image_embeds is [numImageTokens, hidden])
  qwen2vl_lm.onnx      -- language decoder :
                          (inputs_embeds, position_ids, past_key_values)
                              -> logits, present_key_values
                          logits are REAL vocab logits (lm_head applied), shape
                          [1, seq, vocab]; KV is flattened to a single tensor so
                          the on-device engine can thread it as one float blob.
  qwen2vl_embed.bin    -- raw float32 token-embedding matrix [vocab, hidden],
                          row-major, little-endian (no header). The engine embeds
                          text tokens on the CPU and splices the vision
                          embeddings in, so it needs this table.
  model_meta.json      -- { hidden_size, vocab_size, image_token_id } read by
                          MSLiteEngine::Load to size/validate the table.

WHY THIS SHAPE
--------------
The on-device engine (services/screen_parser/inference/src/ms_lite_engine.cpp)
fuses the screenshot via the *inputs_embeds* contract: it looks up text-token
embeddings, replaces the image placeholder token with the vision embeddings, and
feeds the resulting [seq, hidden] matrix to the language graph. That is why the
language model must (a) accept inputs_embeds rather than input_ids, (b) apply
lm_head so the engine's argmax sees real vocab logits, and (c) expose a KV cache
so decoding is O(1) per token instead of recomputing the whole prompt.

The earlier version of this script exported `model.model` (no lm_head -> hidden
states, not logits) with a bare (input_ids, position) signature and no KV, which
did not match the engine and silently dropped the image. Keep the two in sync.

NOTES / TUNING
--------------
* Qwen2-VL's vision tower really consumes (hidden_states, grid_thw) with a
  dynamic patch count. The fixed square `pixel_values` export below is the
  minimal contract the engine binds; for production accuracy export the exact
  visual signature of your checkpoint and adjust MSLiteEngine::EncodeImage.
* KV flattening assumes [num_layers, 2, num_kv_heads, seq, head_dim] ordering.
  Verify against your transformers version; adjust _flatten/_unflatten if the
  past_key_values layout differs.
* Adjust input shapes / opset to the exact checkpoint you convert.

Usage:
  python export_qwen2vl_onnx.py --model Qwen/Qwen2-VL-2B-Instruct --out out
"""

import argparse
import json
import os

import torch
from transformers import AutoModelForVision2Seq, AutoProcessor, AutoTokenizer


# --- Wrappers that expose exactly the I/O the on-device engine binds ---------

class VisionEncoder(torch.nn.Module):
    """pixel_values [1, 3, H, W] -> image_embeds [numImageTokens, hidden]."""

    def __init__(self, model):
        super().__init__()
        self.visual = model.visual

    def forward(self, pixel_values):
        return self.visual(pixel_values)


class LanguageDecoder(torch.nn.Module):
    """(inputs_embeds, position_ids, past_kv_flat) -> (logits, present_kv_flat).

    lm_head is applied (we call the full causal-LM), so `logits` are real vocab
    logits. The KV cache is flattened to/from a single tensor so it crosses the
    ONNX/MindSpore boundary as one float blob the engine can cache.
    """

    def __init__(self, model, num_layers, num_kv_heads, head_dim):
        super().__init__()
        self.model = model
        self.num_layers = num_layers
        self.num_kv_heads = num_kv_heads
        self.head_dim = head_dim

    def _unflatten(self, flat, seq_len):
        if flat is None or flat.numel() == 0:
            return None
        past = []
        per_layer = flat.view(self.num_layers, 2, self.num_kv_heads, seq_len, self.head_dim)
        for i in range(self.num_layers):
            past.append((per_layer[i, 0], per_layer[i, 1]))  # (key, value)
        return tuple(past)

    def _flatten(self, present, seq_len):
        keys, values = [], []
        for layer in present:
            k, v = layer[0], layer[1]
            keys.append(k)
            values.append(v)
        # [num_layers, 2, num_kv_heads, seq, head_dim]
        stacked = torch.stack(
            [torch.stack([k for k in keys]), torch.stack([v for v in values])], dim=1
        )
        return stacked.reshape(-1)

    def forward(self, inputs_embeds, position_ids, past_kv_flat, past_len):
        seq_len = inputs_embeds.shape[1]
        past = self._unflatten(past_kv_flat, past_len)
        out = self.model(
            inputs_embeds=inputs_embeds,
            position_ids=position_ids,
            past_key_values=past,
            use_cache=True,
        )
        present = out.past_key_values
        # present covers past_len + seq_len positions; the engine carries it whole.
        present_flat = self._flatten(present, past_len + seq_len)
        return out.logits, present_flat


# --- Export steps ------------------------------------------------------------

def export_vision(model, out_dir, image_size, opset):
    vision = VisionEncoder(model)
    vision.eval()
    dummy = torch.randn(1, 3, image_size, image_size)
    path = os.path.join(out_dir, "qwen2vl_vision.onnx")
    torch.onnx.export(
        vision,
        (dummy,),
        path,
        input_names=["pixel_values"],
        output_names=["image_embeds"],
        dynamic_axes={"image_embeds": {0: "num_image_tokens"}},
        opset_version=opset,
        do_constant_folding=True,
    )
    print(f"[export] vision -> {path}")


def export_language(model, out_dir, opset, cfg):
    num_layers = cfg.num_hidden_layers
    num_kv_heads = getattr(cfg, "num_key_value_heads", cfg.num_attention_heads)
    head_dim = cfg.hidden_size // cfg.num_attention_heads
    decoder = LanguageDecoder(model, num_layers, num_kv_heads, head_dim)
    decoder.eval()

    seq = 8
    dummy_embeds = torch.randn(1, seq, cfg.hidden_size)
    dummy_pos = torch.arange(seq, dtype=torch.long).unsqueeze(0)  # [1, seq]
    dummy_past = torch.zeros(0)                                    # empty on prefill
    dummy_past_len = torch.zeros(1, dtype=torch.long)

    path = os.path.join(out_dir, "qwen2vl_lm.onnx")
    torch.onnx.export(
        decoder,
        (dummy_embeds, dummy_pos, dummy_past, dummy_past_len),
        path,
        input_names=["inputs_embeds", "position_ids", "past_key_values", "past_len"],
        output_names=["logits", "present_key_values"],
        dynamic_axes={
            "inputs_embeds": {0: "batch", 1: "seq"},
            "position_ids": {0: "batch", 1: "seq"},
            "logits": {0: "batch", 1: "seq"},
            "past_key_values": {0: "past"},
            "present_key_values": {0: "present"},
        },
        opset_version=opset,
        do_constant_folding=True,
    )
    print(f"[export] language -> {path}")


def export_embed_table(model, tokenizer, out_dir, cfg, image_token_id):
    """Dump the token-embedding matrix (raw float32) + model_meta.json."""
    embed = model.get_input_embeddings().weight.detach().to(torch.float32).contiguous()
    vocab_size, hidden_size = embed.shape[0], embed.shape[1]
    bin_path = os.path.join(out_dir, "qwen2vl_embed.bin")
    with open(bin_path, "wb") as f:
        f.write(embed.numpy().tobytes(order="C"))
    print(f"[export] embed table [{vocab_size}, {hidden_size}] -> {bin_path}")

    meta = {
        "hidden_size": int(hidden_size),
        "vocab_size": int(vocab_size),
        "image_token_id": int(image_token_id),
    }
    meta_path = os.path.join(out_dir, "model_meta.json")
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    print(f"[export] meta -> {meta_path}: {meta}")


def resolve_image_token_id(model, tokenizer, fallback=151655):
    """Find the <|image_pad|> id from config or tokenizer."""
    cfg = getattr(model, "config", None)
    for attr in ("image_token_id", "image_token_index"):
        val = getattr(cfg, attr, None)
        if isinstance(val, int):
            return val
    for tok_str in ("<|image_pad|>", "<image>", "<|vision_pad|>"):
        tid = tokenizer.convert_tokens_to_ids(tok_str)
        if isinstance(tid, int) and tid >= 0 and tid != getattr(tokenizer, "unk_token_id", None):
            return tid
    return fallback


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="Qwen/Qwen2-VL-2B-Instruct")
    parser.add_argument("--out", default="out")
    parser.add_argument("--image-size", type=int, default=448)
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    AutoProcessor.from_pretrained(args.model, trust_remote_code=True)  # noqa: F841
    tokenizer = AutoTokenizer.from_pretrained(args.model, trust_remote_code=True)
    model = AutoModelForVision2Seq.from_pretrained(
        args.model, torch_dtype=torch.float32, trust_remote_code=True
    )
    model.eval()

    cfg = model.config
    # Some checkpoints nest the text config; prefer it for LM dims.
    text_cfg = getattr(cfg, "text_config", cfg)

    image_token_id = resolve_image_token_id(model, tokenizer)

    export_vision(model, args.out, args.image_size, args.opset)
    export_language(model, args.out, args.opset, text_cfg)
    export_embed_table(model, tokenizer, args.out, text_cfg, image_token_id)
    print("[export] done. Next: run convert_to_ms.sh to produce .ms models.")


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""Export Qwen2-VL into two ONNX graphs for MindSpore Lite conversion.

Run this on the development machine (needs torch, transformers, onnx). It
produces:
  out/qwen2vl_vision.onnx  -- vision encoder: pixel_values -> image embeds
  out/qwen2vl_lm.onnx      -- language model: (input_ids, position, past_kv)
                              -> logits, present_kv

The two graphs are split because the on-device engine runs them as separate
MindSpore Lite sessions (see MSLiteEngine). Adjust input shapes / opset to the
exact Qwen2-VL checkpoint you convert.

Usage:
  python export_qwen2vl_onnx.py --model Qwen/Qwen2-VL-2B-Instruct --out out
"""

import argparse
import os

import torch
from transformers import AutoModelForVision2Seq, AutoProcessor


def export_vision(model, out_dir, image_size, opset):
    """Export the vision tower with a fixed square input."""
    vision = model.visual
    vision.eval()

    dummy = torch.randn(1, 3, image_size, image_size)
    path = os.path.join(out_dir, "qwen2vl_vision.onnx")
    torch.onnx.export(
        vision,
        (dummy,),
        path,
        input_names=["pixel_values"],
        output_names=["image_embeds"],
        opset_version=opset,
        do_constant_folding=True,
    )
    print(f"[export] vision -> {path}")


def export_language(model, out_dir, opset):
    """Export the language decoder with an explicit KV-cache interface.

    The graph is traced for a single decode step (seq_len=1) plus a prefill
    variant is recommended for production; here we keep a dynamic seq axis.
    """
    lm = model.model if hasattr(model, "model") else model
    lm.eval()

    input_ids = torch.zeros(1, 8, dtype=torch.long)
    position = torch.zeros(1, dtype=torch.long)
    path = os.path.join(out_dir, "qwen2vl_lm.onnx")
    torch.onnx.export(
        lm,
        (input_ids, position),
        path,
        input_names=["input_ids", "position"],
        output_names=["logits"],
        dynamic_axes={
            "input_ids": {0: "batch", 1: "seq"},
            "logits": {0: "batch", 1: "seq"},
        },
        opset_version=opset,
        do_constant_folding=True,
    )
    print(f"[export] language -> {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="Qwen/Qwen2-VL-2B-Instruct")
    parser.add_argument("--out", default="out")
    parser.add_argument("--image-size", type=int, default=448)
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    processor = AutoProcessor.from_pretrained(args.model, trust_remote_code=True)  # noqa: F841
    model = AutoModelForVision2Seq.from_pretrained(
        args.model, torch_dtype=torch.float32, trust_remote_code=True
    )
    model.eval()

    export_vision(model, args.out, args.image_size, args.opset)
    export_language(model, args.out, args.opset)
    print("[export] done. Next: run convert_to_ms.sh to produce .ms models.")


if __name__ == "__main__":
    main()
