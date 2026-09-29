"""Run a reproducible VLM smoke test on Enrico screenshots."""

import argparse
import base64
import hashlib
import json
import os
import time
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path

from openai import OpenAI
from PIL import Image

from benchmarks.enrico_data import sample_rows
from screen.analyzer import SYSTEM_PROMPT, ScreenAnalyzer, ScreenAnalyzerConfig
from screen.hdc import Screenshot
from screen.ocr import OcrError, PaddleOcrReader
from screen.vision import OpenCvCandidateDetector, VisionError, candidate_boxes


RESULTS_DIR = Path(__file__).resolve().parent / "results"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.getenv("SCREEN_VLM_BASE_URL", "http://127.0.0.1:8000/v1"))
    parser.add_argument("--model", default=os.getenv("SCREEN_VLM_MODEL"))
    parser.add_argument("--limit", type=int, default=3)
    parser.add_argument("--ids", nargs="*", default=[])
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--max-tokens", type=int, default=8192)
    parser.add_argument("--ocr", action="store_true", help="include PaddleOCR hints and merge missed text")
    parser.add_argument("--cv", action="store_true", help="include OpenCV candidate boxes")
    parser.add_argument("--json-mode", action=argparse.BooleanOptionalAction, default=True, help="request constrained JSON output")
    parser.add_argument("--cv-max-candidates", type=int, default=40)
    args = parser.parse_args()
    if args.limit < 1:
        parser.error("--limit must be positive")

    client = OpenAI(base_url=args.base_url, api_key=os.getenv("SCREEN_VLM_API_KEY", "EMPTY"), timeout=args.timeout, max_retries=0)
    model = args.model or client.models.list().data[0].id
    rows = sample_rows(args.limit, set(args.ids))
    if not rows:
        parser.error("no matching Enrico screenshots found")

    if args.ocr:
        os.environ.setdefault("PADDLE_PDX_CACHE_HOME", str(Path(__file__).resolve().parent.parent / ".paddlex"))
    ocr_reader = PaddleOcrReader() if args.ocr else None
    cv_detector = OpenCvCandidateDetector(args.cv_max_candidates) if args.cv else None
    analyzer = ScreenAnalyzer(
        ScreenAnalyzerConfig(model_name=model, ocr_enabled=False, cv_enabled=False),
        client=client,
    )
    RESULTS_DIR.mkdir(exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = RESULTS_DIR / f"enrico_smoke_{timestamp}.jsonl"
    failures = 0
    with output.open("w", encoding="utf-8") as destination:
        for row in rows:
            image_bytes = row["image_bytes"]
            with Image.open(BytesIO(image_bytes)) as image:
                width, height = image.size
            started = time.monotonic()
            record = {"screen_id": row["screen_id"], "topic": row["topic"], "model": model, "base_url": args.base_url, "ocr_enabled": args.ocr, "cv_enabled": args.cv, "json_mode": args.json_mode, "max_tokens": args.max_tokens, "prompt_sha256": hashlib.sha256(SYSTEM_PROMPT.encode("utf-8")).hexdigest()}
            ocr_texts = []
            if ocr_reader is not None:
                try:
                    ocr_texts = ocr_reader.read(Screenshot(base64.b64encode(image_bytes).decode("ascii"), width, height))
                except OcrError as error:
                    record["ocr_error"] = str(error)
                record["ocr_count"] = len(ocr_texts)
            user_text = "解析此截图。"
            if cv_detector is not None:
                try:
                    candidates = cv_detector.detect(image_bytes)
                    record["cv_candidates"] = candidate_boxes(candidates)
                    user_text = analyzer._append_cv_hints(user_text, candidates)
                except VisionError as error:
                    record["cv_error"] = str(error)
            if ocr_texts:
                hints = [{"text": item.text, "bounds": item.bounds.__dict__} for item in ocr_texts[:80]]
                user_text += "\nPaddleOCR 参考结果（可能有误，坐标为 0-1000）：" + json.dumps(hints, ensure_ascii=False)
            try:
                response = client.chat.completions.create(
                    model=model,
                    messages=[
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": [
                            {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64," + base64.b64encode(image_bytes).decode("ascii")}},
                            {"type": "text", "text": user_text},
                        ]},
                    ],
                    temperature=0.0,
                    max_tokens=args.max_tokens,
                    stream=False,
                    **({"response_format": {"type": "json_object"}} if args.json_mode else {}),
                )
                record["finish_reason"] = response.choices[0].finish_reason
                content = analyzer._get_message_text(response.choices[0].message)
                record["raw_response"] = content
                raw = analyzer._extract_json(content)
                analysis = analyzer._normalize_analysis(raw, "unknown", width, height)
                record["model_node_count"] = len(analysis.nodes)
                if ocr_texts:
                    analyzer._merge_ocr_texts(analysis.nodes, ocr_texts)
                record["analysis"] = analysis.to_dict()
                record["node_count"] = len(analysis.nodes)
            except Exception as error:
                failures += 1
                record["error"] = f"{type(error).__name__}: {error}"
            finally:
                if "content" in locals():
                    del content
            record["latency_seconds"] = round(time.monotonic() - started, 3)
            destination.write(json.dumps(record, ensure_ascii=False) + "\n")
            print(f"{row['screen_id']}: {record.get('node_count', 'ERROR')} nodes, {record['latency_seconds']}s")
            if "error" in record:
                print(record["error"])
    print(f"Results: {output}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
