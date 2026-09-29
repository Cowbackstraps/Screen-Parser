"""OpenCV proposals for visible graphic regions in a screenshot."""

from dataclasses import dataclass

import cv2
import numpy as np

from screen.schemas import Bounds


class VisionError(RuntimeError):
    """The screenshot cannot be decoded for visual proposals."""


@dataclass(frozen=True)
class VisionCandidate:
    bounds: Bounds
    score: float


class OpenCvCandidateDetector:
    """Find visual regions without assigning accessibility semantics."""

    def __init__(self, max_candidates: int = 40, max_side: int = 1280):
        if max_candidates < 1 or max_side < 1:
            raise ValueError("OpenCV candidate limits must be positive")
        self.max_candidates = max_candidates
        self.max_side = max_side

    def detect(self, image_bytes: bytes) -> list[VisionCandidate]:
        try:
            image = cv2.imdecode(
                np.frombuffer(image_bytes, dtype=np.uint8), cv2.IMREAD_GRAYSCALE
            )
        except cv2.error as error:
            raise VisionError("OpenCV 无法解码截图") from error
        if image is None:
            raise VisionError("OpenCV 无法解码截图")
        original_height, original_width = image.shape
        scale = min(1.0, self.max_side / max(original_width, original_height))
        if scale < 1.0:
            image = cv2.resize(image, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
        height, width = image.shape

        edges = cv2.Canny(image, 50, 150, L2gradient=True)
        edges = cv2.morphologyEx(edges, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
        contours, _ = cv2.findContours(edges, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)

        candidates: list[VisionCandidate] = []
        for contour in contours:
            x, y, box_width, box_height = cv2.boundingRect(contour)
            if box_width < 6 or box_height < 6:
                continue
            if box_width > width * 0.4 or box_height > height * 0.16:
                continue
            box_area = box_width * box_height
            contour_area = cv2.contourArea(contour)
            if contour_area < 12 or contour_area / box_area < 0.05:
                continue
            bounds = Bounds.from_pixels(
                (x / scale, y / scale, (x + box_width) / scale, (y + box_height) / scale),
                original_width,
                original_height,
            )
            if bounds is None:
                continue
            compactness = min(box_width, box_height) / max(box_width, box_height)
            score = contour_area / box_area + 0.25 * compactness
            candidates.append(VisionCandidate(bounds, score))

        candidates.sort(key=lambda item: item.score, reverse=True)
        selected: list[VisionCandidate] = []
        for candidate in candidates:
            if any(self._iou(candidate.bounds, item.bounds) >= 0.65 for item in selected):
                continue
            selected.append(candidate)
            if len(selected) >= self.max_candidates:
                break
        return sorted(selected, key=lambda item: (item.bounds.y1, item.bounds.x1))

    @staticmethod
    def _iou(first: Bounds, second: Bounds) -> float:
        overlap_width = max(0, min(first.x2, second.x2) - max(first.x1, second.x1))
        overlap_height = max(0, min(first.y2, second.y2) - max(first.y1, second.y1))
        intersection = overlap_width * overlap_height
        first_area = (first.x2 - first.x1) * (first.y2 - first.y1)
        second_area = (second.x2 - second.x1) * (second.y2 - second.y1)
        union = first_area + second_area - intersection
        return intersection / union if union else 0.0


def candidate_boxes(candidates: list[VisionCandidate]) -> list[list[int]]:
    return [[item.bounds.x1, item.bounds.y1, item.bounds.x2, item.bounds.y2] for item in candidates]
