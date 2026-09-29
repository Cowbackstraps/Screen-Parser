import unittest

from benchmarks.enrico_metrics import score


class EnricoMetricsTest(unittest.TestCase):
    def test_scores_mapped_types_and_parent_edge(self):
        truth = [
            {"id": 0, "type": "LIST_ITEM", "bounds": {"x1": 0, "y1": 0, "x2": 500, "y2": 200}, "parent_id": None},
            {"id": 1, "type": "TEXT", "bounds": {"x1": 20, "y1": 20, "x2": 200, "y2": 60}, "parent_id": 0},
            {"id": 2, "type": None, "bounds": {"x1": 600, "y1": 0, "x2": 900, "y2": 200}, "parent_id": None},
        ]
        predictions = [
            {"id": "a", "type": "LIST_ITEM", "bounds": truth[0]["bounds"], "parent_id": None},
            {"id": "b", "type": "TEXT", "bounds": truth[1]["bounds"], "parent_id": "a"},
        ]

        result = score(truth, predictions)

        self.assertEqual(result["loc_05"], 2)
        self.assertEqual(result["type_05"], 2)
        self.assertEqual(result["mapped_truth"], 2)
        self.assertEqual(result["parent_edges"], 1)
        self.assertEqual(result["parent_hits"], 1)


if __name__ == "__main__":
    unittest.main()
