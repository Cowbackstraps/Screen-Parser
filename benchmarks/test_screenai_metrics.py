from benchmarks.screenai_metrics import iou, match_count, parse_annotation


def test_parse_annotation_preserves_nested_elements() -> None:
    nodes = parse_annotation(
        "TEXT Hello, world 1 30 2 20, LIST_ITEM 0 100 30 90 "
        "(TEXT First 5 40 35 50, TEXT_INPUT 5 90 55 85)"
    )
    assert len(nodes) == 4
    assert nodes[0]["text"] == "Hello, world"
    assert nodes[0]["bounds"] == (1, 2, 30, 20)
    assert nodes[2]["parent_id"] == nodes[1]["id"]
    assert nodes[3]["type"] == "TEXT_FIELD"


def test_matching_is_one_to_one_and_type_aware() -> None:
    truth = [
        {"type": "TEXT", "bounds": (0, 0, 100, 100)},
        {"type": "BUTTON", "bounds": (200, 0, 300, 100)},
    ]
    predictions = [
        {"type": "TEXT", "bounds": (0, 0, 100, 100)},
        {"type": "TEXT", "bounds": (0, 0, 100, 100)},
        {"type": "TEXT", "bounds": (200, 0, 300, 100)},
    ]
    assert match_count(truth, predictions, 0.5, False) == 2
    assert match_count(truth, predictions, 0.5, True) == 1
    assert iou((0, 0, 100, 100), (50, 50, 150, 150)) == 1 / 7
