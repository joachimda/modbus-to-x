#!/usr/bin/env python3
"""
Local static web server for ESP pages.

- Serves the project 'data' directory at http://127.0.0.1:8000
- Correct MIME types for .js/.mjs
- Adds Cache-Control: no-store to avoid stale assets during development
- Opens the browser automatically (disable with --no-open)
"""

import argparse
import base64
import contextlib
import http.server
import json
import mimetypes
import random
import re
import socket
import socketserver
import sys
import threading
import time
import webbrowser
from functools import partial
from pathlib import Path
from urllib.parse import urlsplit

# Ensure common web types are present
mimetypes.init()
mimetypes.add_type("application/javascript", ".js")
mimetypes.add_type("application/javascript", ".mjs")
mimetypes.add_type("application/json", ".map")
mimetypes.add_type("text/css", ".css")
mimetypes.add_type("image/svg+xml", ".svg")
mimetypes.add_type("text/html; charset=utf-8", ".html")


class NoCacheRequestHandler(http.server.SimpleHTTPRequestHandler):
    ota_password = None
    mqtt_constraints = None
    reboot_pending = False
    reboot_lock = threading.Lock()
    mutation_routes = {
        ("POST", "/api/wifi/connect"),
        ("POST", "/api/wifi/cancel"),
        ("POST", "/api/wifi/ap_off"),
        ("POST", "/api/wifi/reset"),
        ("PUT", "/api/config/modbus"),
        ("PUT", "/api/config/mqtt"),
        ("POST", "/api/config/mqtt/secret"),
        ("POST", "/api/mqtt/test"),
        ("POST", "/api/modbus/execute"),
        ("POST", "/api/modbus/state/disable"),
        ("POST", "/api/modbus/state/enable"),
        ("POST", "/api/system/reboot"),
        ("POST", "/api/system/ota/firmware"),
        ("POST", "/api/system/ota/fs"),
        ("POST", "/api/system/ota/http/check"),
        ("POST", "/api/system/ota/http/notes"),
        ("POST", "/api/system/ota/http/apply"),
        ("POST", "/api/system/ota/http/settings"),
        ("PUT", "/api/system/ota/password"),
        ("DELETE", "/api/system/ota/password"),
        ("POST", "/api/system/factory-reset"),
    }

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

    def _read_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    @classmethod
    def _complete_mock_reboot(cls):
        with cls.reboot_lock:
            cls.reboot_pending = False

    def _normalized_host(self):
        value = self.headers.get("Host", "")
        if not value or any(ch in value for ch in " ,/@[]"):
            return None
        if ":" in value:
            if value.count(":") != 1:
                return None
            value, port = value.rsplit(":", 1)
            if not port.isdigit() or not 1 <= int(port) <= 65535:
                return None
        value = value.lower()
        if not value or value.startswith(".") or value.endswith("."):
            return None
        return value

    def _mutation_context_allowed(self):
        marker_values = self.headers.get_all("X-MBX-Request", failobj=[])
        if marker_values != ["1"]:
            return False
        host = self._normalized_host()
        bound_host = str(self.server.server_address[0]).lower()
        allowed_hosts = {"127.0.0.1", "localhost"}
        if bound_host not in ("", "0.0.0.0"):
            allowed_hosts.add(bound_host)
        return host in allowed_hosts

    def _require_mutation_context(self):
        path = urlsplit(self.path).path
        if (self.command, path) not in self.mutation_routes:
            return True
        if self._mutation_context_allowed():
            return True
        self._send_json({"error": "forbidden_request_context"}, 403)
        return False

    def _ota_authorized(self):
        password = type(self).ota_password
        if password is None:
            return True
        authorization = self.headers.get("Authorization", "")
        if not authorization.startswith("Bearer "):
            return False
        try:
            supplied = base64.b64decode(authorization[7:], validate=True).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return False
        return supplied == password

    def _require_ota_authorized(self):
        if self._ota_authorized():
            return True
        self.send_response(401)
        self.send_header("Content-Type", "application/json")
        self.send_header("WWW-Authenticate", 'Bearer realm="ota"')
        body = b'{"error":"ota_password_required"}'
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return False

    def _handle_api_get(self, path: str) -> bool:
        if path == "/api/config/mqtt/constraints":
            self._send_json(type(self).mqtt_constraints)
            return True

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
                "errorCount": 0,
                # Modbus
                "buses": 1,
                "devices": 1,
                "datapoints": 3,
                "pollIntervalMs": 1000,
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
            body = ("\n".join(lines)).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return True

        if path == "/api/system/ota/http/settings":
            self._send_json({
                "includePrereleases": False,
                "passwordProtected": type(self).ota_password is not None,
            })
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
            send_event("stats-system", {
                "deviceName": "ESP32-DEV",
                "fwVersion": "dev",
                "buildDate": now_iso,
                "chipModel": "ESP32",
                "ip": "192.168.1.42",
                "uptimeMs": int(time.time() * 1000) % (7 * 24 * 3600 * 1000),
                "heapFree": 256000,
                "heapMin": 192000,
                "resetReason": "Power on",
                "cpuFreqMHz": 240,
                "sdkVersion": "IDF-5.x",
            })
            send_event("stats-network", {
                "wifiConnected": True,
                "wifiApMode": False,
                "ssid": "TestNet",
                "ip": "192.168.1.42",
                "rssi": -62,
                "mac": "AA:BB:CC:DD:EE:FF",
            })
            send_event("stats-mqtt", {
                "mqttConnected": True,
                "broker": "mqtt://localhost:1883",
                "clientId": "esp32-dev",
                "mqttErrorCount": 0,
            })
            send_event("stats-modbus", {
                "mbusEnabled": True,
                "buses": 1,
                "devices": 1,
                "datapoints": 3,
                "modbusErrorCount": 0,
            })
            send_event("stats-storage", {
                "flashSize": 4 * 1024 * 1024,
                "spiffsUsed": 512 * 1024,
                "spiffsTotal": 2 * 1024 * 1024,
            })
            send_event("stats-health", {
                "ok": True,
                "components": {
                    "wifi": "ok",
                    "mqtt": "ok",
                    "modbus": "ok",
                    "fs": "ok",
                }
            })
            send_event("logs", {"text": "Stream ready…\n", "truncated": False})
            for i in range(3):
                send_event("log", {"text": f"{time.strftime('%H:%M:%S')} [INFO] Tick {i}\n", "truncated": False})
                time.sleep(1)
            return True

        return False

    def do_GET(self):
        if self.path.startswith("/api/"):
            if self._handle_api_get(self.path):
                return
        return super().do_GET()

    def do_POST(self):
        if not self._require_mutation_context():
            return
        if self.path == "/api/system/reboot":
            handler_type = type(self)
            with handler_type.reboot_lock:
                if handler_type.reboot_pending:
                    self._send_json({"ok": False, "error": "reboot_pending"}, 409)
                    return
                handler_type.reboot_pending = True
                timer = threading.Timer(1.0, handler_type._complete_mock_reboot)
                timer.daemon = True
                try:
                    timer.start()
                except RuntimeError:
                    handler_type.reboot_pending = False
                    self._send_json({"ok": False, "error": "reboot_unavailable"}, 503)
                    return
            self._send_json({"ok": True, "rebooting": True}, 202)
            return
        if self.path == "/api/wifi/reset":
            self._send_json({"ok": True, "resetting": True}, 202)
            return
        if self.path == "/api/system/ota/http/settings":
            self._read_body()
            self.send_response(204)
            self.end_headers()
            return
        if self.path.startswith("/api/system/ota/http/check"):
            if not self._require_ota_authorized():
                return
            self._send_json({"ok": True, "available": True, "pending": False, "version": "9.9.9-test"})
            return
        if self.path.startswith("/api/system/ota/http/notes"):
            if not self._require_ota_authorized():
                return
            self._send_json({"ok": True, "pending": False, "notes": "Mock release notes"})
            return
        if self.path == "/api/system/ota/http/apply":
            if not self._require_ota_authorized():
                return
            self._send_json({"ok": True, "started": True, "version": "9.9.9-test"})
            return
        if self.path in ("/api/system/ota/firmware", "/api/system/ota/fs"):
            if not self._require_ota_authorized():
                self._read_body()
                return
            self._read_body()
            image_type = "firmware" if self.path.endswith("firmware") else "filesystem"
            self._send_json({"ok": True, "type": image_type})
            return
        if self.path == "/api/system/factory-reset":
            if not self._require_ota_authorized():
                self._read_body()
                return
            try:
                payload = json.loads(self._read_body() or b"{}")
            except json.JSONDecodeError:
                payload = {}
            if payload.get("confirm") != "factory-reset":
                self._send_json({"error": "bad_request"}, 400)
                return
            type(self).ota_password = None
            self._send_json({"ok": True, "rebooting": True}, 202)
            return
        return super().do_POST()

    def do_PUT(self):
        if not self._require_mutation_context():
            return
        if self.path == "/api/system/ota/password":
            if not self._require_ota_authorized():
                self._read_body()
                return
            try:
                payload = json.loads(self._read_body() or b"{}")
            except json.JSONDecodeError:
                payload = {}
            password = payload.get("password")
            length = len(password.encode("utf-8")) if isinstance(password, str) else 0
            if length < 8 or length > 128:
                self._send_json({"error": "invalid_password"}, 400)
                return
            type(self).ota_password = password
            self.send_response(204)
            self.end_headers()
            return
        return super().do_PUT()

    def do_DELETE(self):
        if not self._require_mutation_context():
            return
        if self.path == "/api/system/ota/password":
            if not self._require_ota_authorized():
                return
            type(self).ota_password = None
            self.send_response(204)
            self.end_headers()
            return
        self.send_error(404)


def find_project_root(start: Path) -> Path:
    # Walk up until we find the repo root (directory containing this script)
    # Fallback: parent of scripts/
    for p in [start] + list(start.parents):
        if (p / "scripts").is_dir() and (p / "data").is_dir():
            return p
    return start


def load_mqtt_constraints(project_root: Path) -> dict:
    header = project_root / "include" / "mqtt" / "MqttConfigCore.h"
    source = header.read_text(encoding="utf-8")

    def constant(name: str) -> int:
        match = re.search(rf"constexpr\s+(?:size_t|uint16_t)\s+{name}\s*=\s*(\d+)U;", source)
        if not match:
            raise RuntimeError(f"MQTT constraint {name} not found in {header}")
        return int(match.group(1))

    broker_max = constant("BROKER_MAX_BYTES")
    return {
        "brokerIpMaxBytes": broker_max,
        "brokerUrlHostMaxBytes": broker_max,
        "brokerMaxBytes": broker_max,
        "portMin": constant("PORT_MIN"),
        "portMax": constant("PORT_MAX"),
        "portMaxBytes": constant("PORT_MAX_BYTES"),
        "userMaxBytes": constant("USER_MAX_BYTES"),
        "passwordMaxBytes": constant("PASSWORD_MAX_BYTES"),
    }


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
    NoCacheRequestHandler.mqtt_constraints = load_mqtt_constraints(project_root)

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
