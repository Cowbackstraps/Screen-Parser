"""PaddleOCR adapter; the rest of the parser only sees normalized text boxes."""

import base64
import math
from dataclasses import asdict, dataclass
from io import BytesIO
from threading import Lock
from typing import Any

from PIL import Image

from screen.hdc import Screenshot
from screen.schemas import Bounds


class OcrError(RuntimeError):
    """The OCR backend could not analyze this screenshot."""


@dataclass(frozen=True)
class OcrText:
    text: str
    bounds: Bounds
    confidence: float

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class PaddleOcrReader:
    """Read text with PP-OCRv5 mobile models using PaddleOCR's 3.x API."""

    def __init__(self, min_confidence: float = 0.5, pipeline: Any = None):
        self.min_confidence = min_confidence
        self._pipeline = pipeline
        # A single Paddle pipeline instance is reused, and inference is serialized.
        self._lock = Lock()

    def _get_pipeline(self) -> Any:
        if self._pipeline is None:
            try:
                from paddleocr import PaddleOCR
            except ImportError as error:
                raise OcrError(
                    "未安装 PaddleOCR；请先安装 requirements.txt"
                ) from error
            try:
                self._pipeline = PaddleOCR(
                    text_detection_model_name="PP-OCRv5_mobile_det",
                    text_recognition_model_name="PP-OCRv5_mobile_rec",
                    use_doc_orientation_classify=False,
                    use_doc_unwarping=False,
                    use_textline_orientation=False,
                    enable_mkldnn=False,
                )
            except Exception as error:
                raise OcrError(f"PaddleOCR 初始化失败: {error}") from error
        return self._pipeline

    def read(self, screenshot: Screenshot) -> list[OcrText]:
        try:
            import numpy as np

            image_bytes = base64.b64decode(screenshot.base64_data, validate=True)
            with Image.open(BytesIO(image_bytes)) as image:
                # PaddleOCR's ndarray input follows OpenCV's BGR convention.
                pixels = np.asarray(image.convert("RGB"))[:, :, ::-1].copy()
            with self._lock:
                results = self._get_pipeline().predict(pixels)
        except OcrError:
            raise
        except Exception as error:
            raise OcrError(f"PaddleOCR 识别失败: {error}") from error

        if results is None:
            raise OcrError("PaddleOCR 未返回识别结果")
        texts: list[OcrText] = []
        try:
            for result in results:
                data = result.json if hasattr(result, "json") else result
                if not isinstance(data, dict):
                    continue
                data = data.get("res", data)
                if not isinstance(data, dict):
                    continue
                labels = data.get("rec_texts", [])
                scores = data.get("rec_scores", [])
                boxes = data.get("rec_boxes", [])
                for label, score, box in zip(labels, scores, boxes):
                    label = str(label).strip()
                    try:
                        confidence = float(score)
                    except (TypeError, ValueError):
                        continue
                    if not label or not math.isfinite(confidence):
                        continue
                    if confidence < self.min_confidence or confidence > 1:
                        continue
                    bounds = Bounds.from_pixels(box, screenshot.width, screenshot.height)
                    if bounds is None:
                        continue
                    texts.append(OcrText(label[:200], bounds, round(confidence, 3)))
        except Exception as error:
            raise OcrError(f"PaddleOCR 结果格式无法解析: {error}") from error
        return texts
