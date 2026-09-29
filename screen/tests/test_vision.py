import unittest

import cv2
import numpy as np

from screen.vision import OpenCvCandidateDetector, VisionError


class OpenCvCandidateDetectorTest(unittest.TestCase):
    def test_detects_separate_graphic_regions(self):
        image = np.full((800, 400), 255, dtype=np.uint8)
        cv2.rectangle(image, (40, 100), (170, 160), 0, 2)
        cv2.circle(image, (300, 420), 28, 0, 3)
        encoded, buffer = cv2.imencode(".png", image)
        self.assertTrue(encoded)

        candidates = OpenCvCandidateDetector(max_candidates=10).detect(buffer.tobytes())

        self.assertTrue(any(
            item.bounds.x1 <= 125 <= item.bounds.x2
            and item.bounds.y1 <= 163 <= item.bounds.y2
            for item in candidates
        ))
        self.assertTrue(any(
            item.bounds.x1 <= 750 <= item.bounds.x2
            and item.bounds.y1 <= 525 <= item.bounds.y2
            for item in candidates
        ))
        self.assertLessEqual(len(candidates), 10)

    def test_blank_and_invalid_images(self):
        blank = np.full((100, 100), 255, dtype=np.uint8)
        encoded, buffer = cv2.imencode(".png", blank)
        self.assertTrue(encoded)
        detector = OpenCvCandidateDetector()

        self.assertEqual(detector.detect(buffer.tobytes()), [])
        with self.assertRaises(VisionError):
            detector.detect(b"not an image")
        with self.assertRaises(VisionError):
            detector.detect(b"")


if __name__ == "__main__":
    unittest.main()
