"""One-shot semantic analysis for the current HarmonyOS screen."""

import base64
from datetime import datetime, timezone
import json
import os
import re
from dataclasses import dataclass
from typing import Any

from openai import OpenAI

from screen.hdc import get_current_app, get_screenshot, list_devices
from screen.ocr import OcrError, OcrText, PaddleOcrReader
from screen.schemas import Bounds, PageSummary, ScreenAnalysis, ScreenNode
from screen.vision import OpenCvCandidateDetector, VisionCandidate, VisionError, candidate_boxes


ELEMENT_TYPES = {
    "TEXT", "PICTOGRAM", "IMAGE", "BUTTON", "TEXT_FIELD", "LIST_ITEM",
    "SWITCH", "TAB", "NAVIGATION_BAR", "TOOLBAR", "DIALOG", "GROUP", "OTHER",
}
PAGE_TYPES = {"launcher", "settings", "list", "detail", "dialog", "form", "media", "unknown"}
LEGACY_TYPES = {
    "text": "TEXT", "icon": "PICTOGRAM", "image": "IMAGE",
    "button": "BUTTON", "input": "TEXT_FIELD", "list_item": "LIST_ITEM",
    "switch": "SWITCH", "tab": "TAB", "navigation": "NAVIGATION_BAR",
    "dialog": "DIALOG", "other": "OTHER",
}


SYSTEM_PROMPT = """解析手机截图中所有可见的语义 UI 元素。只输出单行紧凑 JSON；首尾必须是 { 和 }，无 Markdown。
顶层键 p 是两个字符串组成的数组：画面上的页面标题、页面类型；n 是节点对象数组。节点键 t 是类型；b 是按整图归一化到 0–1000 的 x1 y1 x2 y2，必须恰好四个整数，以空格分隔成一个字符串；不得使用坐标数组、多点轮廓或像素坐标。s 是可见文字，无文字时省略；d 是无文字图标的简短含义，可省略；p 是此前输出的父节点在 n 中的零基序号，可省略。只使用这些键。
类型：TEXT、PICTOGRAM、IMAGE、BUTTON、TEXT_FIELD、LIST_ITEM、SWITCH、TAB、NAVIGATION_BAR、TOOLBAR、DIALOG、GROUP、OTHER。页面类型：launcher、settings、list、detail、dialog、form、media、unknown。
从顶部到内容区再到底部逐行检查；每个可见文字、图标、图片和控件都给框，包括重复列表行与边角元素。同一元素只输出一次。容器先于子元素输出，不用容器代替子元素；父序号只指真实容器，不能指页面。按钮自身含文字时不再重复建 TEXT。只依据截图标注；OCR 与 OpenCV 是可能有误的定位提示。
使用紧凑 JSON，不换行、不展开坐标、不省略可见元素。"""


class ScreenInputError(RuntimeError):
    """The local device or request cannot be analyzed."""


class ScreenModelError(RuntimeError):
    """The VLM response cannot be converted to the required schema."""


@dataclass
class ScreenAnalyzerConfig:
    base_url: str = "http://127.0.0.1:8000/v1"
    model_name: str = "Qwen3.5-2B"
    api_key: str = "EMPTY"
    timeout: float = 120.0
    max_tokens: int = 8192
    json_mode: bool = True
    pending_threshold: float = 0.68
    cv_enabled: bool = True
    cv_max_candidates: int = 40
    ocr_enabled: bool = True
    ocr_min_score: float = 0.5

    @classmethod
    def from_environment(cls) -> "ScreenAnalyzerConfig":
        return cls(
            base_url=os.getenv("SCREEN_VLM_BASE_URL", "http://127.0.0.1:8000/v1"),
            model_name=os.getenv("SCREEN_VLM_MODEL", "Qwen3.5-2B"),
            api_key=os.getenv("SCREEN_VLM_API_KEY", "EMPTY"),
            timeout=float(os.getenv("SCREEN_VLM_TIMEOUT", "120")),
            max_tokens=int(os.getenv("SCREEN_VLM_MAX_TOKENS", "8192")),
            json_mode=os.getenv("SCREEN_VLM_JSON_MODE", "1").lower()
            in {"1", "true", "yes"},
            pending_threshold=float(os.getenv("SCREEN_PENDING_THRESHOLD", "0.68")),
            cv_enabled=os.getenv("SCREEN_CV_ENABLED", "1").lower()
            in {"1", "true", "yes"},
            cv_max_candidates=int(os.getenv("SCREEN_CV_MAX_CANDIDATES", "40")),
            ocr_enabled=os.getenv("SCREEN_OCR_ENABLED", "1").lower()
            in {"1", "true", "yes"},
            ocr_min_score=float(os.getenv("SCREEN_OCR_MIN_SCORE", "0.5")),
        )


class ScreenAnalyzer:
    """Capture one screen and produce normalized semantic nodes."""

    def __init__(
        self,
        config: ScreenAnalyzerConfig | None = None,
        client: OpenAI | None = None,
        ocr_reader: PaddleOcrReader | None = None,
        cv_detector: OpenCvCandidateDetector | None = None,
    ):
        self.config = config or ScreenAnalyzerConfig.from_environment()
        self.client = client or OpenAI(
            base_url=self.config.base_url,
            api_key=self.config.api_key,
            timeout=self.config.timeout,
        )
        self.ocr_reader = ocr_reader or (
            PaddleOcrReader(self.config.ocr_min_score) if self.config.ocr_enabled else None
        )
        self.cv_detector = cv_detector or (
            OpenCvCandidateDetector(self.config.cv_max_candidates)
            if self.config.cv_enabled else None
        )

    def status(self) -> dict[str, Any]:
        devices = list_devices()
        return {
            "devices": [device.device_id for device in devices],
            "default_device": devices[0].device_id if devices else None,
            "model": self.config.model_name,
            "base_url": self.config.base_url,
            "ocr_enabled": self.ocr_reader is not None,
            "cv_enabled": self.cv_detector is not None,
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

        ocr_texts: list[OcrText] = []
        ocr_error: str | None = None
        if self.ocr_reader is not None:
            try:
                ocr_texts = self.ocr_reader.read(screenshot)
            except OcrError as error:
                ocr_error = str(error)

        cv_candidates: list[VisionCandidate] = []
        cv_error: str | None = None
        if self.cv_detector is not None:
            try:
                image_bytes = base64.b64decode(screenshot.base64_data, validate=True)
                cv_candidates = self.cv_detector.detect(image_bytes)
            except (ValueError, VisionError) as error:
                cv_error = str(error)

        user_text = f"当前前台应用：{current_app}。请解析页面结构、可见语义节点和精确边界。"
        user_text = self._append_cv_hints(user_text, cv_candidates)
        if ocr_texts:
            ocr_hints = [
                {"text": item.text, "bounds": item.bounds.__dict__}
                for item in ocr_texts[:80]
            ]
            user_text += "\nPaddleOCR 参考结果（可能有误，坐标为 0-1000）：" + json.dumps(
                ocr_hints, ensure_ascii=False
            )

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
                            "text": user_text,
                        },
                    ],
                },
            ],
            temperature=0.0,
            max_tokens=self.config.max_tokens,
            stream=False,
            **({"response_format": {"type": "json_object"}} if self.config.json_mode else {}),
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
        self._merge_ocr_texts(analysis.nodes, ocr_texts)
        result = analysis.to_graph_dict(
            timestamp=datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        )
        result.update(
            {
                "device_id": device_id,
                "screenshot": "data:image/png;base64," + screenshot.base64_data,
                "ocr_texts": [item.to_dict() for item in ocr_texts],
                "ocr_error": ocr_error,
                "cv_candidates": candidate_boxes(cv_candidates),
                "cv_error": cv_error,
            }
        )
        return result

    @staticmethod
    def _append_cv_hints(user_text: str, candidates: list[VisionCandidate]) -> str:
        if not candidates:
            return user_text
        coordinates = "; ".join(
            f"{box[0]} {box[1]} {box[2]} {box[3]}"
            for box in candidate_boxes(candidates)
        )
        return user_text + "\nOpenCV 候选区域（每组为 x1 y1 x2 y2，可能误检或漏检）：" + coordinates

    @staticmethod
    def _merge_ocr_texts(nodes: list[ScreenNode], ocr_texts: list[OcrText]) -> None:
        """Add OCR text omitted by the VLM without duplicating labeled controls."""
        used_ids = {node.id for node in nodes}
        for index, item in enumerate(ocr_texts, start=1):
            center_x = (item.bounds.x1 + item.bounds.x2) / 2
            center_y = (item.bounds.y1 + item.bounds.y2) / 2
            label = "".join(item.text.split()).casefold()
            already_named = any(
                node.bounds.x1 <= center_x <= node.bounds.x2
                and node.bounds.y1 <= center_y <= node.bounds.y2
                and label in "".join(node.text.split()).casefold()
                for node in nodes
            )
            if already_named:
                continue
            node_id = f"ocr-{index}"
            while node_id in used_ids:
                node_id += "-ocr"
            used_ids.add(node_id)
            nodes.append(
                ScreenNode(
                    id=node_id,
                    type="TEXT",
                    text=item.text,
                    bounds=item.bounds,
                    confidence=item.confidence,
                    interactive=False,
                    evidence="ocr",
                )
            )

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
        try:
            value = json.loads(cleaned)
        except json.JSONDecodeError as error:
            try:
                value, offset = json.JSONDecoder().raw_decode(cleaned)
                if cleaned[offset:].strip(" \t\r\n]}"):
                    raise error
            except json.JSONDecodeError:
                start = cleaned.find("{")
                end = cleaned.rfind("}")
                if start < 0 or end <= start:
                    raise ScreenModelError(f"VLM JSON 解析失败: {error.msg}") from error
                try:
                    value = json.loads(cleaned[start : end + 1])
                except json.JSONDecodeError as nested_error:
                    raise ScreenModelError(f"VLM JSON 解析失败: {nested_error.msg}") from nested_error
        if isinstance(value, list):
            # Some small VLMs flatten the requested object into one JSON array.
            if value and isinstance(value[0], dict) and isinstance(value[0].get("p"), list):
                return {"p": value[0]["p"], "n": value[0].get("n", value[1:])}
            raise ScreenModelError("VLM JSON 数组缺少页面信息")
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
        compact_page = raw.get("p") if isinstance(raw.get("p"), list) else []
        raw_page = raw.get("page") if isinstance(raw.get("page"), dict) else {}
        if len(compact_page) == 1 and isinstance(compact_page[0], str) and compact_page[0] in PAGE_TYPES:
            compact_page = ["", compact_page[0]]
        page = PageSummary(
            title=str(compact_page[0] if compact_page else raw_page.get("title", ""))[:200],
            page_type=str(compact_page[1] if len(compact_page) > 1 else raw_page.get("page_type", "unknown"))[:80],
            summary=str(raw_page.get("summary", ""))[:500],
        )

        nodes: list[ScreenNode] = []
        used_ids: set[str] = set()
        raw_nodes = raw.get("n") if isinstance(raw.get("n"), list) else raw.get("nodes")
        raw_nodes = raw_nodes if isinstance(raw_nodes, list) else []
        for index, item in enumerate(raw_nodes[:300], start=1):
            if not isinstance(item, dict):
                continue
            bounds = Bounds.from_value(item.get("b", item.get("bounds")), screen_width, screen_height)
            if bounds is None:
                continue

            node_id = str(item.get("id") or f"node-{index}")[:80]
            if node_id in used_ids:
                node_id = f"{node_id}-{index}"
            used_ids.add(node_id)

            try:
                confidence = max(0.0, min(float(item.get("confidence", 0.8)), 1.0))
            except (TypeError, ValueError):
                confidence = 0.0

            element_type = str(item.get("t", item.get("type", ""))).upper()
            if element_type not in ELEMENT_TYPES:
                element_type = LEGACY_TYPES.get(str(item.get("role", "other")), "OTHER")
            legacy_label = item.get("label") if isinstance(item.get("label"), str) else ""
            raw_text = item.get("s", item.get("text"))
            text = raw_text if isinstance(raw_text, str) else (
                legacy_label if element_type not in {"PICTOGRAM", "IMAGE"} else ""
            )
            raw_description = item.get("d", item.get("description"))
            description = raw_description if isinstance(raw_description, str) else ""
            if element_type in {"PICTOGRAM", "IMAGE"} and text and not description:
                description, text = text, ""
            if not text and not description:
                description = legacy_label
            if element_type == "PICTOGRAM" and not text and not description:
                description = "图标，具体含义待确认"
                confidence = min(confidence, max(0.0, self.config.pending_threshold - 0.01))
            evidence = str(item.get("evidence", "vision")).lower()
            if evidence != "vision" or confidence < self.config.pending_threshold:
                evidence = "pending"
            parent_id = item.get("parent_id")
            if type(item.get("p")) is int and 0 <= item["p"] < index - 1:
                candidate = next(
                    (node for node in nodes if node.id == f"node-{item['p'] + 1}"), None
                )
                if candidate and (
                    candidate.bounds.x1 - 5 <= bounds.x1
                    and candidate.bounds.y1 - 5 <= bounds.y1
                    and candidate.bounds.x2 + 5 >= bounds.x2
                    and candidate.bounds.y2 + 5 >= bounds.y2
                ):
                    parent_id = candidate.id
            if not isinstance(parent_id, str) or not parent_id.strip():
                parent_id = None

            interactive = item.get("interactive", item.get("i"))
            if interactive is None:
                interactive = element_type in {"BUTTON", "TEXT_FIELD", "SWITCH", "TAB"}

            nodes.append(
                ScreenNode(
                    id=node_id,
                    type=element_type,
                    text=text[:200],
                    bounds=bounds,
                    confidence=round(confidence, 3),
                    description=description[:500],
                    parent_id=parent_id,
                    interactive=interactive is True,
                    evidence=evidence,
                )
            )

        by_id = {node.id: node for node in nodes}
        for node in nodes:
            seen = {node.id}
            ancestor_id = node.parent_id
            while ancestor_id is not None:
                if ancestor_id not in by_id or ancestor_id in seen:
                    node.parent_id = None
                    break
                seen.add(ancestor_id)
                ancestor_id = by_id[ancestor_id].parent_id

        return ScreenAnalysis(
            page=page,
            nodes=nodes,
            current_app=current_app,
            screen_width=screen_width,
            screen_height=screen_height,
        )
