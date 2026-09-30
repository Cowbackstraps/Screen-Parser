#!/usr/bin/env bash
# Convert the exported ONNX graphs into MindSpore Lite (.ms) models and quantize
# them for on-device inference.
#
# Prerequisites:
#   * MindSpore Lite conversion tool `converter_lite` on PATH (from the
#     mindspore-lite release package).
#   * ONNX graphs produced by export_qwen2vl_onnx.py in ${IN_DIR}.
#
# Outputs (copied to the device at /system/etc/screenparser/):
#   qwen2vl_vision.ms
#   qwen2vl_lm.ms
#   qwen2vl_embed.bin   (token-embedding matrix; MSLiteEngine::Load reads it
#   model_meta.json      from the SAME directory as qwen2vl_lm.ms)
#
# Usage:
#   ./convert_to_ms.sh [in_dir] [out_dir]

set -euo pipefail

IN_DIR="${1:-out}"
OUT_DIR="${2:-ms_out}"
mkdir -p "${OUT_DIR}"

# Post-training int8 quantization config. Tune per-layer for accuracy/size.
QUANT_CONFIG="${IN_DIR}/quant.cfg"
cat > "${QUANT_CONFIG}" <<'CFG'
[common_quant_param]
activation_quant_method=PER_LAYER
activation_quant_algo=KL
activation_quant_param=200

[full_quant]
default_activation_quant_bit=8
default_weight_quant_bit=8

[mixed_bit_param]
# Keep the first/last layers in fp16 to protect accuracy:
# special_op=layer_name_1,layer_name_2
CFG

echo "[convert] vision encoder"
converter_lite \
  --modelFile="${IN_DIR}/qwen2vl_vision.onnx" \
  --fmk=ONNX \
  --saveType=MINDIR \
  --configFile="${QUANT_CONFIG}" \
  --outputFile="${OUT_DIR}/qwen2vl_vision"

echo "[convert] language model"
converter_lite \
  --modelFile="${IN_DIR}/qwen2vl_lm.onnx" \
  --fmk=ONNX \
  --saveType=MINDIR \
  --configFile="${QUANT_CONFIG}" \
  --outputFile="${OUT_DIR}/qwen2vl_lm"

# Carry the CPU-side assets the engine loads next to the language model: the raw
# token-embedding matrix and the metadata (hidden/vocab/image_token ids). Without
# these MSLiteEngine::Load fails with "无法读取词嵌入矩阵" / "model_meta.json 缺失".
for asset in qwen2vl_embed.bin model_meta.json; do
  if [[ -f "${IN_DIR}/${asset}" ]]; then
    cp -f "${IN_DIR}/${asset}" "${OUT_DIR}/${asset}"
    echo "[convert] asset ${asset} -> ${OUT_DIR}/${asset}"
  else
    echo "[convert] WARNING: missing ${IN_DIR}/${asset} (run export_qwen2vl_onnx.py)" >&2
  fi
done

echo "[convert] done -> ${OUT_DIR}"
ls -lh "${OUT_DIR}"
