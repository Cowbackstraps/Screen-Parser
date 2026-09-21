import subprocess
import unittest
from io import BytesIO
from pathlib import Path
from unittest.mock import patch

from PIL import Image

from screen.hdc import get_current_app, get_screenshot, list_devices


class HdcTest(unittest.TestCase):
    def test_list_devices_ignores_empty_marker(self):
        result = subprocess.CompletedProcess([], 0, "[Empty]\n", "")
        with patch("screen.hdc._run", return_value=result):
            self.assertEqual(list_devices(), [])

    def test_get_current_app_returns_foreground_bundle(self):
        output = """Mission ID #1
app name [example.background]
state #BACKGROUND
Mission ID #2
app name [example.foreground]
state #FOREGROUND
"""
        result = subprocess.CompletedProcess([], 0, output, "")
        with patch("screen.hdc._run", return_value=result):
            self.assertEqual(get_current_app("emulator"), "example.foreground")

    def test_get_screenshot_converts_to_png(self):
        image = Image.new("RGB", (20, 30), "red")
        jpeg = BytesIO()
        image.save(jpeg, format="JPEG")

        def run(command, timeout=10):
            if "recv" in command:
                Path(command[-1]).write_bytes(jpeg.getvalue())
            return subprocess.CompletedProcess(command, 0, "", "")

        with patch("screen.hdc._run", side_effect=run):
            screenshot = get_screenshot("emulator")

        self.assertEqual((screenshot.width, screenshot.height), (20, 30))
        self.assertTrue(screenshot.base64_data.startswith("iVBOR"))


if __name__ == "__main__":
    unittest.main()
