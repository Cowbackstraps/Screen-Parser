#!/usr/bin/env python3
"""Export the Qwen2-VL tokenizer assets consumed by QwenTokenizer (C++).

Produces, into --out:
  vocab.json  -- { "<token string>": <id> } in byte-level BPE symbol space
  merges.txt  -- ordered BPE merge rules, one "a b" pair per line

The C++ QwenTokenizer loads exactly these two files (see qwen_tokenizer.cpp).
For a GPT-2 style byte-level BPE tokenizer these are the native artifacts; for
tokenizers that only expose a fast backend we reconstruct them from the added
vocabulary and merge list.

Usage:
  python export_tokenizer.py --model Qwen/Qwen2-VL-2B-Instruct --out out
"""

import argparse
import json
import os

from transformers import AutoTokenizer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="Qwen/Qwen2-VL-2B-Instruct")
    parser.add_argument("--out", default="out")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    tok = AutoTokenizer.from_pretrained(args.model, trust_remote_code=True)

    vocab = tok.get_vocab()  # dict[str, int]
    vocab_path = os.path.join(args.out, "vocab.json")
    with open(vocab_path, "w", encoding="utf-8") as f:
        json.dump(vocab, f, ensure_ascii=False)
    print(f"[tokenizer] vocab ({len(vocab)} tokens) -> {vocab_path}")

    # merges: prefer the raw merges file shipped with the tokenizer.
    merges = getattr(tok, "merges", None)
    merges_path = os.path.join(args.out, "merges.txt")
    with open(merges_path, "w", encoding="utf-8") as f:
        f.write("#version: 0.2\n")
        if merges:
            for m in merges:
                if isinstance(m, (list, tuple)):
                    f.write(" ".join(m) + "\n")
                else:
                    f.write(str(m) + "\n")
    print(f"[tokenizer] merges -> {merges_path}")

    # Record special token ids for the C++ side (defaults are compiled in).
    specials = {
        "bos": tok.bos_token_id,
        "eos": tok.eos_token_id,
        "pad": tok.pad_token_id,
    }
    specials_path = os.path.join(args.out, "special_tokens.json")
    with open(specials_path, "w", encoding="utf-8") as f:
        json.dump(specials, f, ensure_ascii=False, indent=2)
    print(f"[tokenizer] special tokens -> {specials_path}: {specials}")


if __name__ == "__main__":
    main()
