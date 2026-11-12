#!/usr/bin/env python3
"""Loopback-only ESP Serial2WiFi API fixtures; no physical device access or mutations."""

import _paths
import argparse
import copy
import http.server
import json
import mimetypes
from pathlib import Path
import socket
import threading
import time
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
MODES = {"normal", "http-error", "malformed", "delayed", "disconnect"}


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port=0, assets=None):
        self.fixtures = json.loads((ROOT / "tests/support/fixtures/custom-pages.json").read_text())
        self.fixtures.update(json.loads((ROOT / "tests/support/fixtures/api-v1.json").read_text()))
        self.assets = Path(assets).resolve() if assets else None
        self.scenarios = {}
        self.counts = {}
        self.lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(8)
        super().__init__(("127.0.0.1", port), Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # Do not record URLs, drafts or potential credential-bearing input.

    def respond(self, code, body, content_type="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        try:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass  # Client timeout/disconnect is a deliberate fixture scenario.

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        if path != "/__test/scenario":
            self.respond(405, {"error": "Fixture device endpoints are read-only"})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 8192:
                raise ValueError()
            config = json.loads(self.rfile.read(size))
            route = config["route"]
            mode = config["mode"]
            delay = float(config.get("delay", 1.5))
            if route not in self.server.fixtures and route != "*":
                raise ValueError()
            if mode not in MODES or not 0 <= delay <= 5:
                raise ValueError()
            if "response" in config and not route.startswith("/api/v1/"):
                raise ValueError()
        except (ValueError, KeyError, TypeError):
            self.respond(400, {"error": "Invalid scenario"})
            return
        with self.server.lock:
            self.server.scenarios[route] = {"mode": mode, "delay": delay}
            if "response" in config:
                self.server.scenarios[route]["response"] = config["response"]
        self.respond(200, {"fixture": True, "route": route, "mode": mode})

    def do_GET(self):
        if urllib.parse.urlsplit(self.path).path in (
            "/__test/core.js",
            "/__test/management.js",
            "/__test/serial-model.js",
            "/__test/status.js",
        ):
            name = urllib.parse.urlsplit(self.path).path.rsplit("/", 1)[1]
            self.respond(200, (ROOT / "web/app" / name).read_bytes(), "text/javascript")
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            self.send_response(302)
            self.send_header("Location", "/index.html")
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if path.startswith("/api/v1/serial/capture/"):
            path = "/api/v1/serial/capture"
        if path == "/__test/state":
            with self.server.lock:
                state = {
                    "fixture": True,
                    "scenarios": copy.deepcopy(self.server.scenarios),
                    "requests": dict(self.server.counts),
                }
            self.respond(200, state)
            return
        if path in self.server.fixtures:
            with self.server.lock:
                self.server.counts[path] = self.server.counts.get(path, 0) + 1
                scenario = dict(
                    self.server.scenarios.get(
                        path, self.server.scenarios.get("*", {"mode": "normal"})
                    )
                )
            mode = scenario["mode"]
            if mode == "disconnect":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            if mode == "delayed":
                time.sleep(scenario["delay"])
            if mode == "http-error":
                self.respond(503, {"error": "Synthetic device busy"})
            elif mode == "malformed":
                self.respond(200, b'{"incomplete":')
            else:
                self.respond(200, scenario.get("response", self.server.fixtures[path]))
            return
        if self.server.assets:
            relative = urllib.parse.unquote(path).lstrip("/") or "index.html"
            candidate = (self.server.assets / relative).resolve()
            if (
                candidate.is_relative_to(self.server.assets)
                and candidate.is_file()
                and not any(part.startswith(".") for part in Path(relative).parts)
            ):
                self.respond(
                    200,
                    candidate.read_bytes(),
                    mimetypes.guess_type(candidate.name)[0] or "application/octet-stream",
                )
                return
        self.respond(404, {"error": "Fixture route not found"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--assets", type=Path, help="explicit public UI build directory only")
    args = parser.parse_args()
    if args.assets and not args.assets.is_dir():
        parser.error("assets directory does not exist")
    with FixtureServer(args.port, args.assets) as server:
        print(f"Fixture server: http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
