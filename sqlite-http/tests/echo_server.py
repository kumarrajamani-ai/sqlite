#!/usr/bin/env python3
"""Tiny echo server used by the test suite (no external network needed).

GET  /status/<code>   -> returns that HTTP status code
GET  /get              -> 200, JSON echo of query string
POST /post             -> 200, JSON echo of request body + headers
GET  /headers          -> 200, JSON echo of received request headers
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        m = re.match(r"^/status/(\d+)$", self.path)
        if m:
            self.send_response(int(m.group(1)))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/headers"):
            self._send_json(200, {"headers": dict(self.headers.items())})
            return
        self._send_json(200, {"path": self.path, "method": "GET"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        self._send_json(200, {
            "method": "POST",
            "body": body,
            "content_type": self.headers.get("Content-Type"),
        })

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8935
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.serve_forever()
