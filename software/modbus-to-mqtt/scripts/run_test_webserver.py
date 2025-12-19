#!/usr/bin/env python3
"""
Local static web server for ESP pages.

- Serves the project 'data' directory at http://127.0.0.1:8000
- Correct MIME types for .js/.mjs
- Adds Cache-Control: no-store to avoid stale assets during development
- Opens the browser automatically (disable with --no-open)
"""

import argparse
import contextlib
import http.server
import json
import mimetypes
import random
import socket
import socketserver
import sys
import threading
import time
import webbrowser
from functools import partial
from pathlib import Path

# Ensure common web types are present
mimetypes.init()
mimetypes.add_type("application/javascript", ".js")
mimetypes.add_type("application/javascript", ".mjs")
mimetypes.add_type("application/json", ".map")
mimetypes.add_type("text/css", ".css")
mimetypes.add_type("image/svg+xml", ".svg")
mimetypes.add_type("text/html; charset=utf-8", ".html")


class NoCacheRequestHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, log_format, *args):
        sys.stderr.write("%s - - [%s] %s\n" %
                         (self.client_address[0],
                          self.log_date_time_string(),
                          log_format % args))

    # --- Simple API emulation for local testing ---
    def _send_json(self, obj, code=200):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle_api_get(self, path: str) -> bool:
        # System stats used by index.js
        if path == "/api/stats/system":
            now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            # Minimal but realistic payload that index.js expects
            payload = {
                "deviceName": "ESP32-DEV",
                "fwVersion": "dev",
                "buildDate": now_iso,
                "chipModel": "ESP32",
                "chipRevision": 1,
                "cpuFreqMHz": 240,
                "sdkVersion": "IDF-5.x",
                "uptimeMs": int(time.time() * 1000) % (7*24*3600*1000),
                "heapFree": 256000,
                "heapMin": 196000,
                "resetReason": "Power on",
                # Network
                "connected": True,
                "apMode": False,
                "ssid": "TestNet",
                "ip": "192.168.1.42",
                "rssi": -62,
                "mac": "AA:BB:CC:DD:EE:FF",
                # MQTT
                "broker": "mqtt://localhost:1883",
                "clientId": "esp32-dev",
                "lastPublishIso": now_iso,
                "errorCount": 0,
                # Modbus
                "buses": 1,
                "devices": 1,
                "datapoints": 3,
                "pollIntervalMs": 1000,
                "lastPollIso": now_iso,
            }
            self._send_json(payload)
            return True;

        # Logs endpoint used by index
        if path == "/api/logs":
            levels = ["INFO", "WARN", "ERROR", "DEBUG"]
            msgs = [
                "System boot complete",
                "Connected to WiFi TestNet",
                "MQTT connected to mqtt://localhost:1883",
                "Polling datapoints...",
                "Read holding register @100 = 42",
                "Modbus timeout, retrying",
                "Config saved to SPIFFS",
            ]
            def line():
                ts = time.strftime("%H:%M:%S", time.localtime())
                lvl = random.choices(levels, weights=[6,2,1,3], k=1)[0]
                msg = random.choice(msgs)
                return f"{ts} [{lvl}] {msg}"

            lines = [line() for _ in range(20)]
            # Return plain text for simplicity
            body = ("\n".join(lines)).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return True

        if path == "/api/system/reboot":
            self._send_json({"ok": True})
            return True

        if path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()

            def send_event(name: str, obj):
                payload = json.dumps(obj)
                chunk = f"event: {name}\ndata: {payload}\n\n".encode("utf-8")
                self.wfile.write(chunk)
                self.wfile.flush()

            now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            send_event("stats", {
                "deviceName": "ESP32-DEV",
                "fwVersion": "dev",
                "buildDate": now_iso,
                "chipModel": "ESP32",
                "ip": "192.168.1.42",
                "wifiConnected": True,
                "mqttConnected": True,
                "mbusEnabled": True,
                "uptimeMs": int(time.time() * 1000) % (7 * 24 * 3600 * 1000),
            })
            send_event("logs", {"text": "Stream ready…\n", "truncated": False})
            for i in range(3):
                send_event("log", {"text": f"{time.strftime('%H:%M:%S')} [INFO] Tick {i}\n", "truncated": False})
                time.sleep(1)
            return True

        return False

    def do_GET(self):
        # Intercept simple API calls; otherwise serve static files
        if self.path.startswith("/api/"):
            if self._handle_api_get(self.path):
                return
        return super().do_GET()

    def do_POST(self):
        if self.path == "/api/system/reboot":
            self._send_json({"ok": True})
            return
        return super().do_POST()


def find_project_root(start: Path) -> Path:
    # Walk up until we find the repo root (directory containing this script)
    # Fallback: parent of scripts/
    for p in [start] + list(start.parents):
        if (p / "scripts").is_dir() and (p / "data").is_dir():
            return p
    return start


def wait_for_port(host: str, port: int, timeout: float = 3.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
            sock.settimeout(0.25)
            try:
                if sock.connect_ex((host, port)) == 0:
                    return True
            except OSError:
                pass
        time.sleep(0.1)
    return False


def main():
    parser = argparse.ArgumentParser(description="Start local dev web server for ESP pages.")
    parser.add_argument("--host", default="127.0.0.1", help="Host/interface to bind (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8000, help="Port to listen on (default: 8000)")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser automatically")
    parser.add_argument("--dir", default=None, help="Directory to serve (default: project 'data' folder)")
    args = parser.parse_args()

    script_path = Path(__file__).resolve()
    project_root = find_project_root(script_path.parent)
    data_dir = Path(args.dir).resolve() if args.dir else (project_root / "data").resolve()

    if not data_dir.is_dir():
        print(f"[ERR] Data directory not found: {data_dir}", file=sys.stderr)
        sys.exit(1)

    handler_cls = partial(NoCacheRequestHandler, directory=str(data_dir))

    class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
        allow_reuse_address = True
        daemon_threads = True

    with ThreadedTCPServer((args.host, args.port), handler_cls) as httpd:
        url = f"http://{args.host}:{args.port}/"
        print(f"[OK] Serving '{data_dir}' at {url}")
        print("[TIP] Use absolute URLs like /js/app.js and /style.css in your pages.")
        print("[TIP] Press Ctrl+C to stop.")

        if not args.no_open:
            # Try to open the browser after the port is ready
            def _open():
                if wait_for_port(args.host, args.port, timeout=3.0):
                    webbrowser.open(url)
            threading.Thread(target=_open, daemon=True).start()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[INFO] Shutting down server...")
        finally:
            httpd.shutdown()

if __name__ == "__main__":
    main()
