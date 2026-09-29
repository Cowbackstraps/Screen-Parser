import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from screen.hdc import Screenshot
from screen.analyzer import ScreenAnalyzer, ScreenAnalyzerConfig
from screen.ocr import OcrError, OcrText
from screen.schemas import Bounds
from screen.vision import VisionCandidate, VisionError


class ScreenAnalyzerTest(unittest.TestCase):
    def test_ocr_and_cv_enabled_by_default_with_env_opt_out(self):
        with patch.dict("os.environ", {}, clear=True):
            config = ScreenAnalyzerConfig.from_environment()
        self.assertTrue(config.ocr_enabled)
        self.assertTrue(config.cv_enabled)
        self.assertTrue(ScreenAnalyzerConfig().ocr_enabled)
        self.assertTrue(ScreenAnalyzerConfig().cv_enabled)

        with patch.dict("os.environ", {"SCREEN_OCR_ENABLED": "0", "SCREEN_CV_ENABLED": "0"}):
            config = ScreenAnalyzerConfig.from_environment()
        self.assertFalse(config.ocr_enabled)
        self.assertFalse(config.cv_enabled)

    def test_compact_dense_output_preserves_parent_and_boxes(self):
        analyzer = ScreenAnalyzer(ScreenAnalyzerConfig(), client=SimpleNamespace())
        raw = {"p": ["列表", "list"], "n": [
            {"t": "LIST_ITEM", "b": "20 100 980 240"},
            {"t": "TEXT", "b": "40 120 480 155", "s": "第一行", "p": 0},
            {"t": "PICTOGRAM", "b": "900 140 950 190", "d": "更多", "p": 0},
        ]}

        result = analyzer._normalize_analysis(raw, "app", 540, 960)

        self.assertEqual(result.page.title, "列表")
        self.assertEqual(len(result.nodes), 3)
        self.assertEqual(result.nodes[1].bounds, Bounds(40, 120, 480, 155))
        self.assertEqual(result.nodes[1].parent_id, result.nodes[0].id)
        self.assertEqual(result.nodes[2].description, "更多")

    def test_flat_compact_array_is_read_without_brace_slicing(self):
        raw = ScreenAnalyzer._extract_json(
            '[{"p":["首页","launcher"]},{"t":"TEXT","b":"10 20 80 40","s":"欢迎"}]'
        )
        self.assertEqual(raw["p"], ["首页", "launcher"])
        self.assertEqual(raw["n"][0]["b"], "10 20 80 40")

    def test_compact_array_with_stray_closer_and_embedded_nodes(self):
        raw = ScreenAnalyzer._extract_json(
            '[{"p":["首页","launcher"],"n":[{"t":"TEXT","b":[10,20,80,40],"s":"欢迎"}]}]}'
        )
        self.assertEqual(len(raw["n"]), 1)
        self.assertEqual(raw["n"][0]["s"], "欢迎")

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
                        "type": "LIST_ITEM",
                        "text": "无线局域网",
                        "bounds": [40, 100, 960, 180],
                        "confidence": 0.94,
                        "interactive": True,
                        "evidence": "vision",
                        "parent_id": "missing-parent",
                    },
                    {
                        "id": "uncertain",
                        "type": "PICTOGRAM",
                        "text": "",
                        "description": "模糊图标",
                        "bounds": [20, 20, 80, 80],
                        "confidence": 0.4,
                        "interactive": "false",
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
        analyzer = ScreenAnalyzer(
            ScreenAnalyzerConfig(ocr_enabled=False, cv_enabled=False), client=client
        )

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
        self.assertEqual(result["nodes"][0]["sources"], ["vlm"])
        self.assertEqual(result["schema_version"], "4.0")
        self.assertEqual(result["nodes"][0]["type"], "LIST_ITEM")
        self.assertEqual(result["nodes"][0]["text"], "无线局域网")
        self.assertIsNone(result["nodes"][0]["parent_id"])
        self.assertNotIn("aria_role", result["nodes"][0])
        self.assertNotIn("actions", result["nodes"][0])
        self.assertEqual(result["nodes"][1]["sources"], ["vlm"])
        self.assertFalse(result["nodes"][1]["interactive_inferred"])
        self.assertIsNone(result["nodes"][1]["confidence_calibrated"])
        self.assertEqual(result["nodes"][1]["description"], "模糊图标")
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

    def test_cv_hints_are_optional_and_do_not_enable_ocr(self):
        requests = []
        response = SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(
            content='{"page":{"title":"设置"},"nodes":[]}'
        ))])
        client = SimpleNamespace(chat=SimpleNamespace(completions=SimpleNamespace(
            create=lambda **kwargs: requests.append(kwargs) or response
        )))
        detector = SimpleNamespace(detect=lambda image: [
            VisionCandidate(Bounds(10, 20, 30, 40), 0.8)
        ])
        analyzer = ScreenAnalyzer(
            ScreenAnalyzerConfig(max_tokens=8192, ocr_enabled=False),
            client=client,
            cv_detector=detector,
        )
        with (
            patch("screen.analyzer.list_devices", return_value=[SimpleNamespace(device_id="emulator")]),
            patch("screen.analyzer.get_screenshot", return_value=Screenshot("aW1hZ2U=", 800, 1600)),
            patch("screen.analyzer.get_current_app", return_value="设置"),
        ):
            result = analyzer.analyze()

        user_text = requests[0]["messages"][1]["content"][1]["text"]
        self.assertIn("10 20 30 40", user_text)
        self.assertNotIn("PaddleOCR", user_text)
        self.assertEqual(requests[0]["max_tokens"], 8192)
        self.assertEqual(requests[0]["response_format"], {"type": "json_object"})
        self.assertEqual(result["cv_candidates"], [[10, 20, 30, 40]])
        self.assertEqual(result["ocr_texts"], [])

    def test_cv_failure_preserves_vlm_result(self):
        response = SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(
            content='{"page":{"title":"桌面"},"nodes":[]}'
        ))])
        client = SimpleNamespace(chat=SimpleNamespace(completions=SimpleNamespace(
            create=lambda **_: response
        )))

        def fail_cv(_):
            raise VisionError("decode failed")

        analyzer = ScreenAnalyzer(
            ScreenAnalyzerConfig(ocr_enabled=False), client=client,
            cv_detector=SimpleNamespace(detect=fail_cv),
        )
        with (
            patch("screen.analyzer.list_devices", return_value=[SimpleNamespace(device_id="emulator")]),
            patch("screen.analyzer.get_screenshot", return_value=Screenshot("aW1hZ2U=", 800, 1600)),
            patch("screen.analyzer.get_current_app", return_value="桌面"),
        ):
            result = analyzer.analyze()

        self.assertEqual(result["page"]["title"], "桌面")
        self.assertEqual(result["cv_candidates"], [])
        self.assertEqual(result["cv_error"], "decode failed")

    def test_ocr_guides_vlm_and_adds_only_missing_text(self):
        content = json.dumps(
            {
                "page": {"title": "设置", "page_type": "settings"},
                "nodes": [
                    {
                        "id": "wifi",
                        "label": "无线局域网",
                        "role": "list_item",
                        "bounds": [0, 0, 1000, 300],
                        "confidence": 0.9,
                    }
                ],
            },
            ensure_ascii=False,
        )
        response = SimpleNamespace(
            choices=[SimpleNamespace(message=SimpleNamespace(content=content))]
        )
        requests = []
        client = SimpleNamespace(
            chat=SimpleNamespace(
                completions=SimpleNamespace(
                    create=lambda **kwargs: requests.append(kwargs) or response
                )
            )
        )
        ocr_texts = [
            OcrText("无线局域网", Bounds(100, 100, 300, 150), 0.95),
            OcrText("蓝牙", Bounds(100, 400, 200, 450), 0.92),
        ]
        reader = SimpleNamespace(read=lambda screenshot: ocr_texts)
        analyzer = ScreenAnalyzer(ScreenAnalyzerConfig(ocr_enabled=True), client, reader)

        with (
            patch(
                "screen.analyzer.list_devices",
                return_value=[SimpleNamespace(device_id="emulator")],
            ),
            patch(
                "screen.analyzer.get_screenshot",
                return_value=Screenshot("aW1hZ2U=", 800, 1600),
            ),
            patch("screen.analyzer.get_current_app", return_value="设置"),
        ):
            result = analyzer.analyze()

        self.assertEqual(len(result["ocr_texts"]), 2)
        self.assertEqual(len(result["nodes"]), 2)
        self.assertEqual(result["nodes"][0]["type"], "LIST_ITEM")
        self.assertEqual(result["nodes"][0]["text"], "无线局域网")
        self.assertEqual(result["nodes"][1]["type"], "TEXT")
        self.assertEqual(result["nodes"][1]["text"], "蓝牙")
        self.assertEqual(result["nodes"][1]["sources"], ["ocr"])
        self.assertFalse(result["nodes"][1]["interactive_inferred"])
        self.assertIn("PaddleOCR 参考结果", requests[0]["messages"][1]["content"][1]["text"])

    def test_ocr_failure_preserves_vlm_analysis(self):
        response = SimpleNamespace(
            choices=[
                SimpleNamespace(
                    message=SimpleNamespace(
                        content='{"page":{"title":"桌面"},"nodes":[]}'
                    )
                )
            ]
        )
        client = SimpleNamespace(
            chat=SimpleNamespace(
                completions=SimpleNamespace(create=lambda **_: response)
            )
        )

        def fail_ocr(_):
            raise OcrError("模型不可用")

        analyzer = ScreenAnalyzer(
            ScreenAnalyzerConfig(ocr_enabled=True),
            client,
            SimpleNamespace(read=fail_ocr),
        )
        with (
            patch(
                "screen.analyzer.list_devices",
                return_value=[SimpleNamespace(device_id="emulator")],
            ),
            patch(
                "screen.analyzer.get_screenshot",
                return_value=Screenshot("aW1hZ2U=", 800, 1600),
            ),
            patch("screen.analyzer.get_current_app", return_value="桌面"),
        ):
            result = analyzer.analyze()

        self.assertEqual(result["page"]["title"], "桌面")
        self.assertEqual(result["ocr_texts"], [])
        self.assertEqual(result["ocr_error"], "模型不可用")

    def test_parent_links_reject_cycles(self):
        analyzer = ScreenAnalyzer(ScreenAnalyzerConfig(), client=SimpleNamespace())
        raw = {"nodes": [
            {"id": "a", "bounds": [0, 0, 100, 100], "parent_id": "b"},
            {"id": "b", "bounds": [0, 0, 200, 200], "parent_id": "a"},
            {"id": "c", "bounds": [0, 0, 50, 50], "parent_id": "b"},
        ]}
        nodes = analyzer._normalize_analysis(raw, "app", 800, 1600).nodes
        self.assertIsNone(nodes[0].parent_id)
        self.assertEqual(nodes[1].parent_id, "a")
        self.assertEqual(nodes[2].parent_id, "b")

    def test_unlabeled_icon_is_pending_even_with_high_model_score(self):
        analyzer = ScreenAnalyzer(ScreenAnalyzerConfig(), client=SimpleNamespace())
        raw = {"nodes": [{
            "type": "PICTOGRAM", "text": "", "bounds": [10, 10, 60, 60],
            "confidence": 0.99, "interactive": True,
        }]}

        node = analyzer._normalize_analysis(raw, "app", 800, 1600).nodes[0]

        self.assertEqual(node.evidence, "pending")
        self.assertLess(node.confidence, analyzer.config.pending_threshold)
        self.assertIn("待确认", node.description)


if __name__ == "__main__":
    unittest.main()
