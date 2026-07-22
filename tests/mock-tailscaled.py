#!/usr/bin/env python3
"""Minimal tailscaled-LocalAPI-Mock über einen Unix-Socket.

Startet im Zustand NeedsLogin. Ein POST /localapi/v0/start mit AuthKey
loggt ein, PATCH /localapi/v0/prefs schaltet WantRunning. Alle Requests
werden für die Assertions des Smoke-Tests in eine Logdatei geschrieben.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler
from socketserver import UnixStreamServer

sock_path, log_path = sys.argv[1], sys.argv[2]
state = {"logged_in": False, "want_running": False}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _log_req(self, body=b""):
        with open(log_path, "a") as f:
            f.write("%s %s %s\n" % (self.command, self.path, body.decode(errors="replace")))

    def _reply(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def do_GET(self):
        self._log_req()
        if self.path == "/localapi/v0/status":
            if not state["logged_in"]:
                self._reply({"BackendState": "NeedsLogin"})
            elif state["want_running"]:
                self._reply({"BackendState": "Running",
                             "Self": {"Online": True,
                                      "TailscaleIPs": ["100.101.102.103", "fd7a:115c:a1e0::1"]}})
            else:
                self._reply({"BackendState": "Stopped"})
        else:
            self._reply({})

    def do_POST(self):
        body = self._body()
        self._log_req(body)
        if self.path == "/localapi/v0/start":
            opts = json.loads(body or b"{}")
            if opts.get("AuthKey"):
                state["logged_in"] = True
        self._reply({})

    def do_PATCH(self):
        body = self._body()
        self._log_req(body)
        if self.path == "/localapi/v0/prefs":
            prefs = json.loads(body or b"{}")
            if "WantRunning" in prefs:
                state["want_running"] = prefs["WantRunning"]
        self._reply({})

    def log_message(self, *args):
        pass


class Server(UnixStreamServer):
    def get_request(self):
        request, _ = self.socket.accept()
        return request, ("localapi", 0)


if os.path.exists(sock_path):
    os.unlink(sock_path)
Server(sock_path, Handler).serve_forever()
