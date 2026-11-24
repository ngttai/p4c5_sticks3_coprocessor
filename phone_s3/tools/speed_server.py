#!/usr/bin/env python3
"""Server for the network speed test of the phone (Settings > Debug > Network speed, target PC).

GET /down streams zeros until the board closes the connection; POST /up reads and drops the body.
Says "KSDIY-SPEED <port>" by UDP broadcast on port 8081 every second, so the board finds it.
Run on a PC on the same network as the board: python3 speed_server.py [port]
"""
import http.server
import socket
import socketserver
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
CHUNK = b"\0" * 65536


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path != "/down":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(1 << 34))
        self.end_headers()
        sent, t0 = 0, time.time()
        try:
            while True:
                self.wfile.write(CHUNK)
                sent += len(CHUNK)
        except (BrokenPipeError, ConnectionResetError):
            pass
        report("down", sent, t0, self.client_address[0])
        self.close_connection = True

    def do_POST(self):
        if self.path != "/up":
            self.send_error(404)
            return
        left = int(self.headers.get("Content-Length", "0"))
        got, t0 = 0, time.time()
        try:
            while left > 0:
                data = self.rfile.read1(min(65536, left))
                if not data:
                    break
                got += len(data)
                left -= len(data)
        except (ConnectionResetError, TimeoutError):
            pass
        report("up", got, t0, self.client_address[0])
        self.close_connection = True


def report(kind, n, t0, who):
    dt = max(time.time() - t0, 1e-3)
    print(f"{who} {kind}: {n / 1e6:.1f} MB in {dt:.1f} s = {n * 8 / dt / 1e6:.2f} Mbps", flush=True)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def announce():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        try:
            u.sendto(f"KSDIY-SPEED {PORT}".encode(), ("255.255.255.255", 8081))
        except OSError:
            pass
        time.sleep(1)


threading.Thread(target=announce, daemon=True).start()
print(f"speed server on port {PORT}, announced on UDP 8081", flush=True)
Server(("0.0.0.0", PORT), Handler).serve_forever()
