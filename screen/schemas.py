"""Structured output models for screen understanding."""

from dataclasses import asdict, dataclass, field
from typing import Any


@dataclass
class Bounds:
    """A normalized bounding box in the 0-1000 coordinate space."""

    x1: int
    y1: int
    x2: int
    y2: int

    @classmethod
    def from_value(
        cls, value: Any, screen_width: int, screen_height: int
    ) -> "Bounds | None":
        if isinstance(value, dict):
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
    label: str
    role: str
    bounds: Bounds
    confidence: float
    interactive: bool = False
    evidence: str = "vision"
    description: str = ""


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
        return asdict(self)
