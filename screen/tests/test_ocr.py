import base64
import importlib.util
import unittest
from io import BytesIO
from types import SimpleNamespace

from PIL import Image

from screen.hdc import Screenshot
from screen.ocr import PaddleOcrReader


class PaddleOcrReaderTest(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("numpy"), "可选 OCR 依赖未安装")
    def test_converts_pixel_boxes_and_filters_weak_or_invalid_text(self):
        image = Image.new("RGB", (800, 1600), "white")
        buffer = BytesIO()
        image.save(buffer, format="PNG")
        screenshot = Screenshot(base64.b64encode(buffer.getvalue()).decode(), 800, 1600)

        captured = []
        result = SimpleNamespace(
            json={
                "res": {
                    "rec_texts": ["设置", "低分", " "],
                    "rec_scores": [0.95, 0.2, 0.99],
                    "rec_boxes": [[80, 160, 240, 320], [20, 20, 60, 60], [50, 50, 90, 90]],
                }
            }
        )
        pipeline = SimpleNamespace(predict=lambda pixels: captured.append(pixels) or [result])
        texts = PaddleOcrReader(min_confidence=0.5, pipeline=pipeline).read(screenshot)

        self.assertEqual(len(texts), 1)
        self.assertEqual(texts[0].text, "设置")
        self.assertEqual(texts[0].bounds.__dict__, {"x1": 100, "y1": 100, "x2": 300, "y2": 200})
        self.assertEqual(captured[0].shape, (1600, 800, 3))


if __name__ == "__main__":
    unittest.main()
