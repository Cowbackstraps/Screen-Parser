"""One-shot semantic analysis for the current HarmonyOS screen."""

import json
import os
import re
from dataclasses import dataclass
from typing import Any

from openai import OpenAI

from screen.hdc import get_current_app, get_screenshot, list_devices
from screen.schemas import Bounds, PageSummary, ScreenAnalysis, ScreenNode


SYSTEM_PROMPT = """你是鸿蒙系统界面语义解析器。分析给定截图并只返回一个 JSON 对象，不要输出 Markdown 或解释。

JSON 格式：
{
  "page": {
    "title": "页面标题",
    "page_type": "launcher|settings|list|detail|dialog|form|media|unknown",
    "summary": "一句话描述页面用途"
  },
  "nodes": [
    {
      "id": "node-1",
      "label": "屏幕上可见的文字或简短语义名称",
      "role": "button|text|input|icon|image|list_item|switch|tab|navigation|dialog|other",
      "bounds": [x1, y1, x2, y2],
      "confidence": 0.0,
      "interactive": true,
      "evidence": "vision|pending",
      "description": "节点用途"
    }
  ]
}

规则：
1. bounds 使用 0-1000 归一化坐标，原点在左上角，边框必须紧贴元素。
2. 优先识别可交互控件、页面标题、列表项、输入框及关键状态文本。
3. 不要把多个独立控件合并成一个大框，不要输出屏幕外节点。
4. 仅凭截图能够直接确认的节点 evidence=vision；被遮挡、边界模糊或语义不确定的节点 evidence=pending。
5. 无法确认时降低 confidence，严禁猜测不存在的控件。
"""


class ScreenInputError(RuntimeError):
    """The local device or request cannot be analyzed."""


class ScreenModelError(RuntimeError):
    """The VLM response cannot be converted to the required schema."""


@dataclass
class ScreenAnalyzerConfig:
    base_url: str = "http://127.0.0.1:8000/v1"
    model_name: str = "qwen3.5-2b"
    api_key: str = "EMPTY"
    timeout: float = 120.0
    pending_threshold: float = 0.68

    @classmethod
    def from_environment(cls) -> "ScreenAnalyzerConfig":
        return cls(
            base_url=os.getenv("SCREEN_VLM_BASE_URL", "http://127.0.0.1:8000/v1"),
            model_name=os.getenv("SCREEN_VLM_MODEL", "qwen3.5-2b"),
            api_key=os.getenv("SCREEN_VLM_API_KEY", "EMPTY"),
            timeout=float(os.getenv("SCREEN_VLM_TIMEOUT", "120")),
            pending_threshold=float(os.getenv("SCREEN_PENDING_THRESHOLD", "0.68")),
        )


class ScreenAnalyzer:
    """Capture one screen and produce normalized semantic nodes."""

    def __init__(
        self,
        config: ScreenAnalyzerConfig | None = None,
        client: OpenAI | None = None,
    ):
        self.config = config or ScreenAnalyzerConfig.from_environment()
        self.client = client or OpenAI(
            base_url=self.config.base_url,
            api_key=self.config.api_key,
            timeout=self.config.timeout,
        )

    def status(self) -> dict[str, Any]:
        devices = list_devices()
        return {
            "devices": [device.device_id for device in devices],
            "default_device": devices[0].device_id if devices else None,
            "model": self.config.model_name,
            "base_url": self.config.base_url,
        }

    def analyze(self, device_id: str | None = None) -> dict[str, Any]:
        devices = list_devices()
        available_ids = [device.device_id for device in devices]
        if not device_id:
            if not available_ids:
                raise ScreenInputError("未检测到 HDC 设备或模拟器")
            device_id = available_ids[0]
        elif device_id not in available_ids:
            raise ScreenInputError(f"HDC 目标不可用: {device_id}")

        screenshot = get_screenshot(device_id)
        if screenshot.is_sensitive:
            raise ScreenInputError("当前页面禁止截图，无法进行视觉解析")

        try:
            current_app = get_current_app(device_id)
        except Exception:
            current_app = "unknown"

        response = self.client.chat.completions.create(
            model=self.config.model_name,
            messages=[
                {"role": "system", "content": SYSTEM_PROMPT},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": "data:image/png;base64," + screenshot.base64_data
                            },
                        },
                        {
                            "type": "text",
                            "text": (
                                f"当前前台应用：{current_app}。"
                                "请解析页面结构、可见语义节点和精确边界。"
                            ),
                        },
                    ],
                },
            ],
            temperature=0.0,
            max_tokens=4000,
            stream=False,
        )
        if not response.choices:
            raise ScreenModelError("VLM 返回了空响应")

        content = self._get_message_text(response.choices[0].message)
        raw = self._extract_json(content)
        analysis = self._normalize_analysis(
            raw,
            current_app=current_app,
            screen_width=screenshot.width,
            screen_height=screenshot.height,
        )
        result = analysis.to_dict()
        result.update(
            {
                "device_id": device_id,
                "screenshot": "data:image/png;base64," + screenshot.base64_data,
            }
        )
        return result

    @staticmethod
    def _get_message_text(message: Any) -> str:
        """Read normal content and reasoning-style output used by some VLLM models."""
        candidates = [getattr(message, "content", None), getattr(message, "reasoning", None)]
        model_extra = getattr(message, "model_extra", None)
        if isinstance(model_extra, dict):
            candidates.extend(
                [model_extra.get("reasoning"), model_extra.get("reasoning_content")]
            )

        for candidate in candidates:
            if isinstance(candidate, str) and candidate.strip():
                return candidate
            if isinstance(candidate, list):
                text_parts = [
                    str(part.get("text", ""))
                    for part in candidate
                    if isinstance(part, dict) and part.get("type") == "text"
                ]
                joined = "\n".join(part for part in text_parts if part).strip()
                if joined:
                    return joined
        raise ScreenModelError(
            "VLM 未返回文本结果；请确认所选模型支持图像输入和 Chat Completions"
        )

    @staticmethod
    def _extract_json(content: str) -> dict[str, Any]:
        cleaned = re.sub(r"^\s*```(?:json)?\s*|\s*```\s*$", "", content.strip())
        start = cleaned.find("{")
        end = cleaned.rfind("}")
        if start < 0 or end <= start:
            raise ScreenModelError("VLM 响应中没有 JSON 对象")
        try:
            value = json.loads(cleaned[start : end + 1])
        except json.JSONDecodeError as error:
            raise ScreenModelError(f"VLM JSON 解析失败: {error.msg}") from error
        if not isinstance(value, dict):
            raise ScreenModelError("VLM 响应必须是 JSON 对象")
        return value

    def _normalize_analysis(
        self,
        raw: dict[str, Any],
        current_app: str,
        screen_width: int,
        screen_height: int,
    ) -> ScreenAnalysis:
        raw_page = raw.get("page") if isinstance(raw.get("page"), dict) else {}
        page = PageSummary(
            title=str(raw_page.get("title", ""))[:200],
            page_type=str(raw_page.get("page_type", "unknown"))[:80],
            summary=str(raw_page.get("summary", ""))[:500],
        )

        nodes: list[ScreenNode] = []
        used_ids: set[str] = set()
        raw_nodes = raw.get("nodes") if isinstance(raw.get("nodes"), list) else []
        for index, item in enumerate(raw_nodes[:300], start=1):
            if not isinstance(item, dict):
                continue
            bounds = Bounds.from_value(item.get("bounds"), screen_width, screen_height)
            if bounds is None:
                continue

            node_id = str(item.get("id") or f"node-{index}")[:80]
            if node_id in used_ids:
                node_id = f"{node_id}-{index}"
            used_ids.add(node_id)

            try:
                confidence = max(0.0, min(float(item.get("confidence", 0.0)), 1.0))
            except (TypeError, ValueError):
                confidence = 0.0

            evidence = str(item.get("evidence", "vision")).lower()
            if evidence != "vision" or confidence < self.config.pending_threshold:
                evidence = "pending"

            nodes.append(
                ScreenNode(
                    id=node_id,
                    label=str(item.get("label", "未命名节点"))[:200],
                    role=str(item.get("role", "other"))[:80],
                    bounds=bounds,
                    confidence=round(confidence, 3),
                    interactive=bool(item.get("interactive", False)),
                    evidence=evidence,
                    description=str(item.get("description", ""))[:500],
                )
            )

        return ScreenAnalysis(
            page=page,
            nodes=nodes,
            current_app=current_app,
            screen_width=screen_width,
            screen_height=screen_height,
        )
