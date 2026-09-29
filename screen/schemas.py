"""Structured output models for screen understanding."""

from dataclasses import asdict, dataclass, field
import math
import re
from typing import Any


@dataclass
class Bounds:
    """A normalized bounding box in the 0-1000 coordinate space."""

    x1: int
    y1: int
    x2: int
    y2: int

    @classmethod
    def from_pixels(
        cls, value: Any, screen_width: int, screen_height: int
    ) -> "Bounds | None":
        """Convert an explicit pixel box to the shared 0-1000 coordinate space."""
        if screen_width <= 0 or screen_height <= 0:
            return None
        try:
            x1, y1, x2, y2 = [float(item) for item in value]
        except (TypeError, ValueError):
            return None
        if not all(math.isfinite(item) for item in (x1, y1, x2, y2)):
            return None
        return cls.from_value(
            [
                max(0, min(x1 / screen_width * 1000, 1000)),
                max(0, min(y1 / screen_height * 1000, 1000)),
                max(0, min(x2 / screen_width * 1000, 1000)),
                max(0, min(y2 / screen_height * 1000, 1000)),
            ],
            screen_width,
            screen_height,
        )

    @classmethod
    def from_value(
        cls, value: Any, screen_width: int, screen_height: int
    ) -> "Bounds | None":
        if isinstance(value, str):
            parts = re.fullmatch(r"\s*(\d{1,4})[ ,]+(\d{1,4})[ ,]+(\d{1,4})[ ,]+(\d{1,4})\s*", value)
            if parts is None:
                return None
            values = parts.groups()
        elif isinstance(value, dict):
            values = [
                value.get("x1"),
                value.get("y1"),
                value.get("x2"),
                value.get("y2"),
            ]
        elif isinstance(value, (list, tuple)) and len(value) == 4:
            values = list(value)
        else:
            return None

        try:
            coordinates = [float(item) for item in values]
        except (TypeError, ValueError):
            return None

        # Accept pixel coordinates as a defensive fallback.
        if coordinates[2] > 1000 or coordinates[3] > 1000:
            if screen_width <= 0 or screen_height <= 0:
                return None
            coordinates = [
                coordinates[0] / screen_width * 1000,
                coordinates[1] / screen_height * 1000,
                coordinates[2] / screen_width * 1000,
                coordinates[3] / screen_height * 1000,
            ]

        x1, y1, x2, y2 = [round(item) for item in coordinates]
        x1, x2 = sorted((max(0, min(x1, 1000)), max(0, min(x2, 1000))))
        y1, y2 = sorted((max(0, min(y1, 1000)), max(0, min(y2, 1000))))
        if x2 - x1 < 2 or y2 - y1 < 2:
            return None
        return cls(x1=x1, y1=y1, x2=x2, y2=y2)


@dataclass
class ScreenNode:
    id: str
    type: str
    text: str
    bounds: Bounds
    confidence: float
    description: str = ""
    parent_id: str | None = None
    interactive: bool = False
    evidence: str = "vision"


@dataclass
class PageSummary:
    title: str = ""
    page_type: str = "unknown"
    summary: str = ""


@dataclass
class ScreenAnalysis:
    page: PageSummary
    nodes: list[ScreenNode] = field(default_factory=list)
    current_app: str = "unknown"
    screen_width: int = 0
    screen_height: int = 0

    def to_dict(self) -> dict[str, Any]:
        """Legacy compact form used by offline benchmark scripts."""
        result = asdict(self)
        result["schema_version"] = "3.0"
        return result

    def to_graph_dict(self, *, timestamp: str | None = None) -> dict[str, Any]:
        """Serialize the public ScreenGraph snapshot described in the project report.

        Unknown system facts and uncalibrated confidence are represented as null.
        Only directly known containment edges are emitted.
        """
        version = "4.0"
        children: dict[str, list[str]] = {node.id: [] for node in self.nodes}
        edges: list[dict[str, str]] = []
        for node in self.nodes:
            if node.parent_id in children and node.parent_id != node.id:
                children[node.parent_id].append(node.id)
                edges.append({
                    "type": "contains",
                    "source_id": node.parent_id,
                    "target_id": node.id,
                })

        sources = {"vision": "vlm", "pending": "vlm", "ocr": "ocr", "manual": "fixture"}
        graph_nodes: list[dict[str, Any]] = []
        for node in self.nodes:
            bounds_norm = asdict(node.bounds)
            bounds_px = {
                "x1": round(node.bounds.x1 * self.screen_width / 1000),
                "y1": round(node.bounds.y1 * self.screen_height / 1000),
                "x2": round(node.bounds.x2 * self.screen_width / 1000),
                "y2": round(node.bounds.y2 * self.screen_height / 1000),
            }
            graph_nodes.append({
                "id": node.id,
                "type": node.type,
                "text": node.text,
                "description": node.description,
                "bounds_px": bounds_px,
                "bounds_norm": bounds_norm,
                "window_id": None,
                "parent_id": node.parent_id if node.parent_id in children else None,
                "relations": [
                    {"type": "contains", "target_id": child_id}
                    for child_id in children[node.id]
                ],
                "visible": True,
                "occluded": None,
                "interactive_inferred": node.interactive,
                "actions_verified": [],
                "confidence_calibrated": None,
                "sources": [sources.get(node.evidence, node.evidence)],
                "evidence_refs": ["screenshot"],
                "timestamp": timestamp,
                "schema_version": version,
            })

        return {
            "schema_version": version,
            "page": asdict(self.page),
            "nodes": graph_nodes,
            "edges": edges,
            "current_app": self.current_app,
            "screen_width": self.screen_width,
            "screen_height": self.screen_height,
        }
