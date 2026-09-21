"""Minimal HarmonyOS HDC adapter for read-only screen understanding.

Adapted from the HDC implementation in zai-org/Open-AutoGLM (Apache-2.0).
Modified for this project: only device discovery, screenshot and foreground app
lookup are retained; Agent actions and app-name mappings are omitted.
"""

import base64
import os
import re
import subprocess
import tempfile
import uuid
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

from PIL import Image


@dataclass(frozen=True)
class DeviceInfo:
    device_id: str


@dataclass(frozen=True)
class Screenshot:
    base64_data: str
    width: int
    height: int
    is_sensitive: bool = False


def _command(device_id: str | None, *args: str) -> list[str]:
    command = [os.getenv("HDC_PATH", "hdc")]
    if device_id:
        command.extend(["-t", device_id])
    return command + list(args)


def _run(command: list[str], timeout: int = 10) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError as error:
        raise RuntimeError("未找到 hdc，请将 DevEco Studio 的 hdc 加入 PATH") from error
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("HDC 命令超时") from error


def list_devices() -> list[DeviceInfo]:
    result = _run(_command(None, "list", "targets"), timeout=5)
    if result.returncode != 0:
        raise RuntimeError(f"HDC 设备查询失败: {result.stderr.strip()}")
    return [
        DeviceInfo(device_id=line.strip())
        for line in result.stdout.splitlines()
        if line.strip() and line.strip().lower() not in {"[empty]", "empty"}
    ]


def get_current_app(device_id: str | None = None) -> str:
    result = _run(_command(device_id, "shell", "aa", "dump", "-l"))
    if result.returncode != 0:
        raise RuntimeError(f"读取前台应用失败: {result.stderr.strip()}")

    current_bundle = None
    for line in result.stdout.splitlines():
        if "Mission ID" in line:
            current_bundle = None
        if "app name [" in line:
            match = re.search(r"app name \[([^\]]+)\]", line)
            if match:
                current_bundle = match.group(1)
        if "state #foreground" in line.lower() and current_bundle:
            return current_bundle
    return "System Home"


def get_screenshot(device_id: str | None = None, timeout: int = 10) -> Screenshot:
    remote_path = f"/data/local/tmp/screen_parser_{uuid.uuid4().hex}.jpeg"
    local_path: Path | None = None
    try:
        result = _run(_command(device_id, "shell", "screenshot", remote_path), timeout)
        output = (result.stdout + result.stderr).lower()
        if result.returncode != 0 or any(
            marker in output for marker in ("fail", "error", "not found")
        ):
            result = _run(
                _command(device_id, "shell", "snapshot_display", "-f", remote_path),
                timeout,
            )
            output = (result.stdout + result.stderr).lower()
            if result.returncode != 0 or any(
                marker in output for marker in ("fail", "error", "not found")
            ):
                raise RuntimeError("HDC 截图失败；当前页面可能禁止截图")

        with tempfile.NamedTemporaryFile(suffix=".jpeg", delete=False) as temp_file:
            local_path = Path(temp_file.name)
        result = _run(
            _command(device_id, "file", "recv", remote_path, str(local_path)),
            timeout,
        )
        if result.returncode != 0 or local_path.stat().st_size == 0:
            raise RuntimeError(f"HDC 截图传输失败: {result.stderr.strip()}")

        with Image.open(local_path) as image:
            width, height = image.size
            buffer = BytesIO()
            image.convert("RGB").save(buffer, format="PNG")
        return Screenshot(
            base64_data=base64.b64encode(buffer.getvalue()).decode("ascii"),
            width=width,
            height=height,
        )
    finally:
        if local_path is not None:
            local_path.unlink(missing_ok=True)
        try:
            _run(_command(device_id, "shell", "rm", "-f", remote_path), timeout=5)
        except RuntimeError:
            pass
