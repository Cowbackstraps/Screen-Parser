"""Read the official Enrico screenshots and semantic view hierarchies."""

import csv
import json
from pathlib import Path
from zipfile import ZipFile

from screen.schemas import Bounds


DATA_DIR = Path(__file__).resolve().parent / "data" / "enrico"
SCREENSHOTS = DATA_DIR / "screenshots.zip"
HIERARCHIES = DATA_DIR / "hierarchies.zip"
TOPICS = DATA_DIR / "design_topics.csv"

# Only these classes have a direct counterpart in the parser schema.
TYPE_MAP = {
    "Text": "TEXT",
    "Image": "IMAGE",
    "Icon": "PICTOGRAM",
    "List Item": "LIST_ITEM",
    "Text Button": "BUTTON",
    "Toolbar": "TOOLBAR",
    "Input": "TEXT_FIELD",
    "Modal": "DIALOG",
    "On/Off Switch": "SWITCH",
    "Bottom Navigation": "NAVIGATION_BAR",
}


def topic_index() -> dict[str, str]:
    with TOPICS.open(encoding="utf-8", newline="") as source:
        return {row["screen_id"]: row["topic"] for row in csv.DictReader(source)}


def sample_rows(limit: int, ids: set[str] | None = None) -> list[dict]:
    """Return deterministic, topic-diverse Enrico examples."""
    topics = topic_index()
    selected = [screen_id for screen_id in sorted(topics, key=int) if not ids or screen_id in ids]
    if ids and len(selected) != len(ids):
        raise ValueError(f"unknown Enrico IDs: {sorted(ids - set(selected))}")
    buckets: dict[str, list[str]] = {}
    for screen_id in selected:
        buckets.setdefault(topics[screen_id], []).append(screen_id)
    order: list[str] = []
    while buckets and len(order) < limit:
        for topic in sorted(tuple(buckets)):
            order.append(buckets[topic].pop(0))
            if not buckets[topic]:
                del buckets[topic]
            if len(order) >= limit:
                break
    with ZipFile(SCREENSHOTS) as images, ZipFile(HIERARCHIES) as hierarchies:
        return [
            {
                "screen_id": screen_id,
                "topic": topics[screen_id],
                "image_bytes": images.read(f"screenshots/{screen_id}.jpg"),
                "hierarchy": json.loads(hierarchies.read(f"hierarchies/{screen_id}.json")),
            }
            for screen_id in order
        ]


def semantic_nodes(hierarchy: dict) -> list[dict]:
    """Keep annotated components and link each to its nearest annotated ancestor."""
    hierarchy = hierarchy.get("activity", {}).get("root", hierarchy)
    root_box = hierarchy.get("bounds", [0, 0, 1440, 2560])
    width = max(1, int(root_box[2]) - int(root_box[0]))
    height = max(1, int(root_box[3]) - int(root_box[1]))
    result: list[dict] = []

    def visit(node: dict, parent_id: int | None) -> None:
        label = node.get("componentLabel")
        box = Bounds.from_pixels(node.get("bounds"), width, height)
        if label and box is not None:
            current_id = len(result)
            result.append({
                "id": current_id,
                "type": TYPE_MAP.get(label),
                "source_type": label,
                "text": node.get("text", ""),
                "bounds": box.__dict__,
                "parent_id": parent_id,
            })
            parent_id = current_id
        for child in node.get("children", []):
            visit(child, parent_id)

    visit(hierarchy, None)
    return result
