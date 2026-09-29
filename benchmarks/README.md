# Evaluation: Enrico

Run `.venv\Scripts\python.exe -m benchmarks.verify_data` to verify all 1,460
official Enrico screenshots and view hierarchies against the topic index.
`Pillow` is required. Downloaded files live under `data/enrico/` and are
ignored by Git.

The active whole-screen benchmark is [Enrico](https://github.com/luileito/enrico).
The downloaded files are the official [screenshots](https://userinterfaces.aalto.fi/enrico/resources/screenshots.zip),
[view hierarchies](https://userinterfaces.aalto.fi/enrico/resources/hierarchies.zip),
[topic index](https://github.com/luileito/enrico/blob/master/design_topics.csv), and
[known issues](https://github.com/luileito/enrico/blob/master/issues.csv).
Three hierarchy files have no semantic component annotations; they remain
available for a connection smoke test but contribute no element ground truth.

Run the connected model, then score its output:

```powershell
.venv\Scripts\python.exe -m benchmarks.smoke_vlm --limit 5
.venv\Scripts\python.exe -m benchmarks.enrico_metrics benchmarks\results\enrico_smoke_<timestamp>.jsonl
```

`--ids` selects specific Enrico screen IDs. Without it, the script samples
deterministically across page topics. Add `--ocr` or `--cv` to evaluate those
optional hints. JSON-constrained output is enabled by default; `--no-json-mode`
disables it for comparison. The metric script reports JSON success, element localization
recall, recall for the 10 Enrico classes that directly map to parser types,
and parent-edge recall. Matching is one-to-one at normalized-box IoU 0.1 or
0.5. This is a project diagnostic, not an official Enrico leaderboard score.
The hierarchy is a proxy for visible elements and has known defects; review
the issue list before drawing aggregate conclusions. Scoring excludes the 81
known-issue screens by default; `--include-known-issues` includes them.

The cached [CAGUI](https://huggingface.co/datasets/openbmb/CAGUI) samples may
still be used for a separate Chinese UI check; they do not annotate a complete
semantic tree. HarmonyOS coverage still needs its own labeled examples.

## 2026-09-29 Enrico pilot

`Qwen3.5-2B`, the compact prompt, JSON response format, no OCR/CV hints,
`max_tokens=8192`, and one deterministic sample per Enrico topic (`--limit 20`):

| Measure | Result |
| --- | ---: |
| Parseable JSON | 20/20 responses; 18/18 after two known-issue screens are excluded |
| Ground-truth / returned nodes | 410 / 265 on the 18 scored screens |
| Localization recall, IoU >= 0.1 / 0.5 | 169/410 (41.2%) / 85/410 (20.7%) |
| Mapped-type recall, IoU >= 0.1 / 0.5 | 131/388 (33.8%) / 66/388 (17.0%) |
| Parent-edge recall, typed IoU >= 0.5 | 0/186 |

Without server-side JSON format, the same 20 samples had 15/18 parseable
responses after issue filtering and 43/388 mapped-type hits at IoU 0.5.
These are small diagnostic runs, not full-dataset performance estimates.
The model still misses many elements and does not reliably recover hierarchy.

## Historical ScreenAI diagnostics

The ScreenAI runs below used older prompts and are no longer the active
benchmark. Enrico overlaps Rico, so their samples are not independent.

| Local file | Source | Purpose |
| --- | --- | --- |
| `data/screenai_test.csv` | [Google Screen Annotation](https://github.com/google-research-datasets/screen_annotation) | Official test annotations |
| `data/screenai_test.parquet` | [RICO-ScreenAnnotation mirror](https://huggingface.co/datasets/bevaya/RICO-ScreenAnnotation) | Same test IDs with embedded screenshots |
| `data/cagui_cap_validation.parquet` | [CAGUI mirror](https://huggingface.co/datasets/cua-lite/CAGUI) | Chinese element-caption validation sample |
| `data/cagui_use_validation.parquet` | [CAGUI mirror](https://huggingface.co/datasets/cua-lite/CAGUI) | Chinese agent validation sample with UI positions |
| `data/cmgui_test.jsonl` | [CMGUI](https://huggingface.co/datasets/alibabagroup/CMGUI) | Chinese app test metadata only; screenshots are not downloaded |

The actual ScreenAI test files contain 4,217 matching screenshot/annotation
pairs. The Parquet SHA-256 is
`f7bb7f0721274758cb72f55f1410c7b39e0bb3a2750cfd8db3d90b0a02520952`.
The annotation is dense, hierarchical screen parsing with element type,
text/description, and 0-999 coordinates. The Parquet file contains the image
bytes, so the 6 GB Rico archive is not needed. The CAGUI validation samples
are useful for Chinese UI checks, but their caption/action labels are not
equivalent to complete screen annotations. CMGUI contains 8 WeChat test
episodes with 54 steps,
but its labels describe task targets, not every visible element; its 4.5 GB
test screenshot archive is not part of this checkout.

Do not mix these datasets into one coverage percentage. WeChat/HarmonyOS
coverage still requires a separate, explicitly annotated test set.

Historical results use `benchmarks.screenai_metrics` and old result files.

## 2026-09-29 pilot

The remote `Qwen3.5-2B` service was reachable through a local port forward at
`http://127.0.0.1:8000/v1` (`max_model_len=4096`). With the current production
prompt, OCR disabled, `max_tokens=2048`, and the first 20 ScreenAI test rows:

| Measure | Result |
| --- | ---: |
| Parseable JSON | 10 / 20 screens |
| Ground-truth / returned nodes | 595 / 80 |
| Type-aware matched nodes, IoU >= 0.1 | 54 / 595 (9.1% recall) |
| Type-aware matched nodes, IoU >= 0.5 | 42 / 595 (7.1% recall) |
| Mean request latency | 6.37 s |

Failed responses count as zero predictions. This is a deterministic small
pilot, not an estimate of full-test performance or HarmonyOS/WeChat coverage.
One failed screenshot (`57160`) was retried with `max_tokens=3000` and still
finished with `length`, producing truncated JSON. The current one-shot verbose
schema is therefore a likely bottleneck on dense screens.

## 2026-09-29 larger-context retest

The same model was restarted with `max_model_len=65536`. The first 20 test
screens were run again with `max_tokens=8192`, first without OCR and then with
PP-OCRv5 mobile text/box hints plus the production OCR merge rule:

| Measure | No OCR | OCR hints + merge |
| --- | ---: | ---: |
| Parseable JSON | 17/20 | 19/20 |
| Returned nodes | 188 | 475 (321 VLM + 154 added OCR) |
| Type-aware recall, IoU >= 0.1 | 137/595 = 23.0% | 248/595 = 41.7% |
| Type-aware recall, IoU >= 0.5 | 106/595 = 17.8% | 180/595 = 30.3% |
| Mean end-to-end latency | 9.56 s | 18.51 s |

The remaining OCR-assisted failed screen (`57160`) still ended with `length`
at `max_tokens=16384`; its incomplete output contained about 143 node records
for 56 ground-truth elements. Larger token budgets alone therefore do not
establish complete-screen coverage. These 20 ordered examples are diagnostic,
not a representative confidence interval for all 4,217 screens.

## 2026-09-29 OpenCV candidate pilot without OCR

The PC prototype now detects up to 40 OpenCV visual proposals per screenshot
and passes their coordinates as optional VLM hints. The VLM must verify them
against the image; proposals are never counted as semantic nodes. Reproduce
the no-OCR run and the proposal-only diagnostic with:

```powershell
.venv\Scripts\python.exe -m benchmarks.smoke_vlm --limit 20 --max-tokens 8192 --cv
.venv\Scripts\python.exe -m benchmarks.cv_metrics --limit 20 --max-candidates 40
```

The same first 20 ScreenAI test screenshots and remote `Qwen3.5-2B` were used:

| Measure | Previous prompt, no OCR | New prompt, no CV/OCR | New prompt + CV, no OCR |
| --- | ---: | ---: | ---: |
| Parseable JSON | 17/20 | 7/20 | 18/20 |
| Returned nodes | 188 | 106 | 402 |
| Type-aware recall, IoU >= 0.1 | 137/595 (23.0%) | 89/595 (15.0%) | 162/595 (27.2%) |
| Type-aware recall, IoU >= 0.5 | 106/595 (17.8%) | 50/595 (8.4%) | 93/595 (15.6%) |
| Leaf localization recall, IoU >= 0.1 | 176/472 (37.3%) | 99/472 (21.0%) | 207/472 (43.9%) |
| Pictogram type-aware recall, IoU >= 0.1 | 12/180 (6.7%) | 5/180 (2.8%) | 24/180 (13.3%) |
| Mean latency | 9.56 s | 25.90 s | 11.67 s |

Separately, 800 OpenCV proposals localized 81/180 pictograms (45.0%) at IoU
0.1, without assigning semantic types. The VLM result remains far below the
95% coverage target, and the stricter IoU 0.5 score regressed. A trial of
`response_format=json_object` on two truncated examples still ended at the
output limit, so it is not enabled. These ordered 20 screens are an exploratory
diagnostic rather than a representative estimate of full-test or Chinese-app
performance.
