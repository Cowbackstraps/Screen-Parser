import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from screen.hdc import Screenshot
from screen.analyzer import ScreenAnalyzer, ScreenAnalyzerConfig


class ScreenAnalyzerTest(unittest.TestCase):
    def test_analyze_returns_normalized_nodes(self):
        content = json.dumps(
            {
                "page": {
                    "title": "设置",
                    "page_type": "settings",
                    "summary": "系统设置页面",
                },
                "nodes": [
                    {
                        "id": "wifi",
                        "label": "无线局域网",
                        "role": "list_item",
                        "bounds": [40, 100, 960, 180],
                        "confidence": 0.94,
                        "interactive": True,
                        "evidence": "vision",
                    },
                    {
                        "id": "uncertain",
                        "label": "模糊图标",
                        "role": "icon",
                        "bounds": [20, 20, 80, 80],
                        "confidence": 0.4,
                        "evidence": "vision",
                    },
                ],
            },
            ensure_ascii=False,
        )
        response = SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content=content))]
        )
        client = SimpleNamespace(
            chat=SimpleNamespace(
                completions=SimpleNamespace(create=lambda **_: response)
            )
        )
        analyzer = ScreenAnalyzer(ScreenAnalyzerConfig(), client=client)

        with (
            patch(
                "screen.analyzer.list_devices",
                return_value=[SimpleNamespace(device_id="emulator")],
            ),
            patch(
                "screen.analyzer.get_screenshot",
                return_value=Screenshot("aW1hZ2U=", 1320, 2856),
            ),
            patch("screen.analyzer.get_current_app", return_value="设置"),
        ):
            result = analyzer.analyze()

        self.assertEqual(result["device_id"], "emulator")
        self.assertEqual(result["page"]["page_type"], "settings")
        self.assertEqual(result["nodes"][0]["evidence"], "vision")
        self.assertEqual(result["nodes"][1]["evidence"], "pending")
        self.assertTrue(result["screenshot"].startswith("data:image/png;base64,"))

    def test_reads_json_from_vllm_reasoning_field(self):
        content = json.dumps(
            {
                "page": {"title": "桌面", "page_type": "launcher"},
                "nodes": [],
            },
            ensure_ascii=False,
        )
        message = SimpleNamespace(content=None, reasoning=content, model_extra=None)

        self.assertEqual(
            ScreenAnalyzer._get_message_text(message),
            content,
        )


if __name__ == "__main__":
    unittest.main()
