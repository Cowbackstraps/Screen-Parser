"""Verify the downloaded Enrico benchmark files."""

from io import BytesIO
import json
from zipfile import ZipFile

from PIL import Image

from benchmarks.enrico_data import HIERARCHIES, SCREENSHOTS, topic_index


def verify_image(image_bytes: bytes) -> None:
    if not image_bytes:
        raise ValueError("empty image bytes")
    with Image.open(BytesIO(image_bytes)) as image:
        image.verify()


def verify_enrico() -> None:
    ids = set(topic_index())
    with ZipFile(SCREENSHOTS) as screenshots, ZipFile(HIERARCHIES) as hierarchies:
        image_ids = {name.removeprefix("screenshots/").removesuffix(".jpg") for name in screenshots.namelist() if name.endswith(".jpg")}
        hierarchy_ids = {name.removeprefix("hierarchies/").removesuffix(".json") for name in hierarchies.namelist() if name.endswith(".json")}
        if ids != image_ids or ids != hierarchy_ids or len(ids) != 1460:
            raise ValueError(f"Enrico ID mismatch: topics={len(ids)}, images={len(image_ids)}, hierarchies={len(hierarchy_ids)}")
        for screen_id in sorted(ids, key=int):
            verify_image(screenshots.read(f"screenshots/{screen_id}.jpg"))
            hierarchy = json.loads(hierarchies.read(f"hierarchies/{screen_id}.json"))
            if "bounds" not in hierarchy and "activity" not in hierarchy:
                raise ValueError(f"missing hierarchy root: {screen_id}")
    print(f"Enrico: {len(ids)} matched topics, screenshots and hierarchies")


if __name__ == "__main__":
    verify_enrico()
