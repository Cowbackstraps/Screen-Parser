"""Local HTTP server for the screen-understanding interface."""

import argparse
import json
import mimetypes
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from screen.analyzer import ScreenAnalyzer, ScreenInputError, ScreenModelError


STATIC_DIR = Path(__file__).resolve().parent / "static"


class ScreenRequestHandler(BaseHTTPRequestHandler):
    analyzer: ScreenAnalyzer

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/status":
            self._send_json(HTTPStatus.OK, self.analyzer.status())
            return

        requested = "index.html" if path == "/" else path.lstrip("/")
        file_path = (STATIC_DIR / requested).resolve()
        if STATIC_DIR not in file_path.parents or not file_path.is_file():
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "资源不存在"})
            return

        content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
        body = file_path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self._send_common_headers(content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/api/analyze":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "接口不存在"})
            return

        try:
            length = min(int(self.headers.get("Content-Length", "0")), 64 * 1024)
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as error:
                raise ScreenInputError("请求必须是有效 JSON") from error
            device_id = payload.get("device_id") if isinstance(payload, dict) else None
            result = self.analyzer.analyze(device_id=device_id)
            self._send_json(HTTPStatus.OK, result)
        except ScreenInputError as error:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
        except ScreenModelError as error:
            self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(error)})
        except Exception as error:
            self._send_json(
                HTTPStatus.BAD_GATEWAY,
                {"error": f"屏幕解析失败: {type(error).__name__}: {error}"},
            )

    def log_message(self, format: str, *args) -> None:
        print(f"[screen] {self.address_string()} - {format % args}")

    def _send_json(self, status: HTTPStatus, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self._send_common_headers("application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_common_headers(self, content_type: str) -> None:
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'",
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="HarmonyOS screen-understanding UI")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    analyzer = ScreenAnalyzer()
    ScreenRequestHandler.analyzer = analyzer
    server = ThreadingHTTPServer((args.host, args.port), ScreenRequestHandler)
    print(f"Screen UI: http://{args.host}:{args.port}")
    print(f"VLM: {analyzer.config.model_name} @ {analyzer.config.base_url}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nScreen UI stopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
