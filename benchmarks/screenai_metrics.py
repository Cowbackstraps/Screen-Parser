"""Parse ScreenAI labels and score element localization on a small run."""

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path


DATA_PATH = Path(__file__).resolve().parent / "data" / "screenai_test.csv"
TYPE_RE = re.compile(r"[A-Z][A-Z_]*")
BOX_RE = re.compile(
    r"\s+(\d{1,4}) (\d{1,4}) (\d{1,4}) (\d{1,4})"
    r"(?=\s*\(|,\s*[A-Z]|\)|$)"
)
TYPE_ALIASES = {"TEXT_INPUT": "TEXT_FIELD"}


def parse_annotation(source: str) -> list[dict]:
    nodes: list[dict] = []

    def parse_sequence(position: int, parent_id: int | None = None) -> int:
        while position < len(source):
            while position < len(source) and source[position].isspace():
                position += 1
            if position < len(source) and source[position] == ")":
                return position + 1
            element = TYPE_RE.match(source, position)
            if element is None:
                raise ValueError(f"expected element type at offset {position}")
            box = BOX_RE.search(source, element.end())
            if box is None:
                raise ValueError(f"expected box after {element.group()} at offset {position}")
            x1, x2, y1, y2 = map(int, box.groups())
            node_id = len(nodes)
            nodes.append({
                "id": node_id,
                "type": TYPE_ALIASES.get(element.group(), element.group()),
                "text": source[element.end():box.start()].strip(),
                "bounds": (x1, y1, x2, y2),
                "parent_id": parent_id,
            })
            position = box.end()
            while position < len(source) and source[position].isspace():
                position += 1
            if position < len(source) and source[position] == "(":
                position = parse_sequence(position + 1, node_id)
            while position < len(source) and source[position].isspace():
                position += 1
            if position < len(source) and source[position] == ",":
                position += 1
            elif position < len(source) and source[position] == ")":
                return position + 1
            elif position != len(source):
                raise ValueError(f"unexpected character at offset {position}")
        return position

    if parse_sequence(0) != len(source):
        raise ValueError("trailing annotation text")
    return nodes


def iou(first: tuple[int, ...], second: tuple[int, ...]) -> float:
    x1 = max(first[0], second[0])
    y1 = max(first[1], second[1])
    x2 = min(first[2], second[2])
    y2 = min(first[3], second[3])
    intersection = max(0, x2 - x1) * max(0, y2 - y1)
    area_a = max(0, first[2] - first[0]) * max(0, first[3] - first[1])
    area_b = max(0, second[2] - second[0]) * max(0, second[3] - second[1])
    union = area_a + area_b - intersection
    return intersection / union if union else 0.0


def match_count(truth: list[dict], predictions: list[dict], threshold: float, type_aware: bool) -> int:
    neighbors: list[list[int]] = []
    for prediction in predictions:
        bounds = prediction["bounds"]
        if isinstance(bounds, dict):
            bounds = tuple(bounds[key] for key in ("x1", "y1", "x2", "y2"))
        candidates = [
            (index, iou(item["bounds"], bounds))
            for index, item in enumerate(truth)
            if not type_aware or item["type"] == prediction["type"]
        ]
        neighbors.append([
            index for index, overlap in sorted(candidates, key=lambda item: -item[1])
            if overlap >= threshold
        ])

    assigned: dict[int, int] = {}

    def augment(prediction_index: int, visited: set[int]) -> bool:
        for truth_index in neighbors[prediction_index]:
            if truth_index in visited:
                continue
            visited.add(truth_index)
            if truth_index not in assigned or augment(assigned[truth_index], visited):
                assigned[truth_index] = prediction_index
                return True
        return False

    return sum(augment(index, set()) for index in range(len(predictions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("predictions", nargs="+", type=Path)
    args = parser.parse_args()
    with DATA_PATH.open(encoding="utf-8", newline="") as source:
        labels = {row["screen_id"]: row["screen_annotation"] for row in csv.DictReader(source)}

    totals = {
        "screens": 0, "valid_json": 0, "truth": 0, "leaves": 0,
        "predictions": 0, "loc_01": 0, "type_01": 0, "type_05": 0,
        "leaf_loc_01": 0, "leaf_type_01": 0,
    }
    truth_types: Counter[str] = Counter()
    matched_types: Counter[str] = Counter()
    for path in args.predictions:
        with path.open(encoding="utf-8") as source:
            for line in source:
                result = json.loads(line)
                truth = parse_annotation(labels[result["screen_id"]])
                parents = {item["parent_id"] for item in truth if item["parent_id"] is not None}
                leaves = [item for item in truth if item["id"] not in parents]
                predictions = result.get("analysis", {}).get("nodes", [])
                counts = {
                    "screens": 1,
                    "valid_json": int("analysis" in result),
                    "truth": len(truth),
                    "leaves": len(leaves),
                    "predictions": len(predictions),
                    "loc_01": match_count(truth, predictions, 0.1, False),
                    "type_01": match_count(truth, predictions, 0.1, True),
                    "type_05": match_count(truth, predictions, 0.5, True),
                    "leaf_loc_01": match_count(leaves, predictions, 0.1, False),
                    "leaf_type_01": match_count(leaves, predictions, 0.1, True),
                }
                for key, value in counts.items():
                    totals[key] += value
                for element_type in {item["type"] for item in truth}:
                    class_truth = [item for item in truth if item["type"] == element_type]
                    class_predictions = [item for item in predictions if item["type"] == element_type]
                    truth_types[element_type] += len(class_truth)
                    matched_types[element_type] += match_count(class_truth, class_predictions, 0.1, True)
                print(f"{result['screen_id']}: {counts}")
    print(f"TOTAL: {totals}")
    if totals["truth"]:
        print(f"parseable JSON: {totals['valid_json']}/{totals['screens']}")
        print(f"localization recall @ IoU 0.1: {totals['loc_01'] / totals['truth']:.3f}")
        print(f"type-aware recall @ IoU 0.1: {totals['type_01'] / totals['truth']:.3f}")
        print(f"type-aware recall @ IoU 0.5: {totals['type_05'] / totals['truth']:.3f}")
        print(f"leaf localization recall @ IoU 0.1: {totals['leaf_loc_01'] / totals['leaves']:.3f}")
        print(f"leaf type-aware recall @ IoU 0.1: {totals['leaf_type_01'] / totals['leaves']:.3f}")
        print("per-type recall @ IoU 0.1:")
        for element_type, count in truth_types.most_common():
            print(f"  {element_type}: {matched_types[element_type]}/{count} ({matched_types[element_type] / count:.3f})")


if __name__ == "__main__":
    main()
