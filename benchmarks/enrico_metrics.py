"""Score whole-screen Enrico element coverage, type and parent links."""

import argparse
import csv
import json
from pathlib import Path
from zipfile import ZipFile

from benchmarks.enrico_data import DATA_DIR, HIERARCHIES, semantic_nodes


def iou(first: tuple[int, ...], second: tuple[int, ...]) -> float:
    x1, y1 = max(first[0], second[0]), max(first[1], second[1])
    x2, y2 = min(first[2], second[2]), min(first[3], second[3])
    intersection = max(0, x2 - x1) * max(0, y2 - y1)
    area_a = max(0, first[2] - first[0]) * max(0, first[3] - first[1])
    area_b = max(0, second[2] - second[0]) * max(0, second[3] - second[1])
    union = area_a + area_b - intersection
    return intersection / union if union else 0.0


def match_pairs(truth: list[dict], predictions: list[dict], threshold: float, typed: bool) -> dict[int, int]:
    """One-to-one matching, returned as truth-index -> prediction-index."""
    neighbors: list[list[int]] = []
    for prediction in predictions:
        box = prediction["bounds"]
        if isinstance(box, dict):
            box = tuple(box[key] for key in ("x1", "y1", "x2", "y2"))
        candidates = [
            (index, iou(tuple(item["bounds"][key] for key in ("x1", "y1", "x2", "y2")), box))
            for index, item in enumerate(truth)
            if not typed or (item["type"] is not None and item["type"] == prediction["type"])
        ]
        neighbors.append([
            index for index, overlap in sorted(candidates, key=lambda pair: -pair[1])
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

    for index in range(len(predictions)):
        augment(index, set())
    return assigned


def score(truth: list[dict], predictions: list[dict]) -> dict[str, int]:
    typed = match_pairs(truth, predictions, 0.5, True)
    parent_edges = [
        node for node in truth
        if node["parent_id"] is not None
        and node["type"] is not None
        and truth[node["parent_id"]]["type"] is not None
    ]
    parent_hits = sum(
        node["id"] in typed
        and node["parent_id"] in typed
        and predictions[typed[node["id"]]].get("parent_id") == predictions[typed[node["parent_id"]]].get("id")
        for node in parent_edges
    )
    return {
        "truth": len(truth),
        "mapped_truth": sum(node["type"] is not None for node in truth),
        "predictions": len(predictions),
        "loc_01": len(match_pairs(truth, predictions, 0.1, False)),
        "loc_05": len(match_pairs(truth, predictions, 0.5, False)),
        "type_01": len(match_pairs(truth, predictions, 0.1, True)),
        "type_05": len(typed),
        "parent_edges": len(parent_edges),
        "parent_hits": parent_hits,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("predictions", nargs="+", type=Path)
    parser.add_argument("--include-known-issues", action="store_true")
    args = parser.parse_args()
    with (DATA_DIR / "issues.csv").open(encoding="utf-8", newline="") as source:
        issue_ids = {row["screen_id"] for row in csv.DictReader(source)}
    totals = {key: 0 for key in (
        "screens", "skipped", "valid_json", "truth", "mapped_truth", "predictions",
        "loc_01", "loc_05", "type_01", "type_05", "parent_edges", "parent_hits",
    )}
    with ZipFile(HIERARCHIES) as hierarchies:
        for path in args.predictions:
            with path.open(encoding="utf-8") as source:
                for line in source:
                    result = json.loads(line)
                    screen_id = result["screen_id"]
                    if screen_id in issue_ids and not args.include_known_issues:
                        totals["skipped"] += 1
                        print(f"{screen_id}: skipped (Enrico known issue)")
                        continue
                    truth = semantic_nodes(json.loads(hierarchies.read(f"hierarchies/{screen_id}.json")))
                    predictions = result.get("analysis", {}).get("nodes", [])
                    counts = score(truth, predictions)
                    counts["screens"] = 1
                    counts["valid_json"] = int("analysis" in result)
                    print(f"{screen_id} ({result.get('topic', '?')}): {counts}")
                    for key, value in counts.items():
                        totals[key] += value
    print(f"TOTAL: {totals}")
    for label, numerator, denominator in (
        ("JSON success", "valid_json", "screens"),
        ("localization recall @ IoU 0.1", "loc_01", "truth"),
        ("localization recall @ IoU 0.5", "loc_05", "truth"),
        ("mapped-type recall @ IoU 0.1", "type_01", "mapped_truth"),
        ("mapped-type recall @ IoU 0.5", "type_05", "mapped_truth"),
        ("parent-edge recall @ typed IoU 0.5", "parent_hits", "parent_edges"),
    ):
        if totals[denominator]:
            print(f"{label}: {totals[numerator]}/{totals[denominator]} = {totals[numerator] / totals[denominator]:.3f}")


if __name__ == "__main__":
    main()
