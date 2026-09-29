"""Measure OpenCV proposal localization without OCR or VLM inference."""

import argparse
from benchmarks.enrico_data import sample_rows, semantic_nodes
from benchmarks.enrico_metrics import match_pairs
from screen.vision import OpenCvCandidateDetector


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--max-candidates", type=int, default=40)
    args = parser.parse_args()
    if args.limit < 1 or args.max_candidates < 1:
        parser.error("limits must be positive")

    detector = OpenCvCandidateDetector(max_candidates=args.max_candidates)
    totals = {"screens": 0, "candidates": 0, "icons": 0, "icon_hits": 0, "leaves": 0, "leaf_hits": 0}
    for row in sample_rows(args.limit, set()):
        truth = semantic_nodes(row["hierarchy"])
        parents = {item["parent_id"] for item in truth if item["parent_id"] is not None}
        leaves = [item for item in truth if item["id"] not in parents]
        icons = [item for item in truth if item["type"] == "PICTOGRAM"]
        proposals = [
            {"type": "PICTOGRAM", "bounds": item.bounds.__dict__}
            for item in detector.detect(row["image_bytes"])
        ]
        totals["screens"] += 1
        totals["candidates"] += len(proposals)
        totals["icons"] += len(icons)
        totals["icon_hits"] += len(match_pairs(icons, proposals, 0.1, False))
        totals["leaves"] += len(leaves)
        totals["leaf_hits"] += len(match_pairs(leaves, proposals, 0.1, False))

    print(totals)
    if totals["icons"]:
        print(f"icon proposal recall @ IoU 0.1: {totals['icon_hits'] / totals['icons']:.3f}")
    if totals["leaves"]:
        print(f"leaf proposal recall @ IoU 0.1: {totals['leaf_hits'] / totals['leaves']:.3f}")


if __name__ == "__main__":
    main()
