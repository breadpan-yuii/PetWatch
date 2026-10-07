#!/usr/bin/env python3
"""PetWatch IoT gateway — no extra hardware required.

The UNO talks to this laptop over the existing USB serial line. This script:
  * reads every line the feeder prints,
  * POSTS notable events (FEED/EATEN/ALERT/WARN/ERR/RTC/OK) as JSON to a
    webhook URL (IFTTT, webhook.site, a local endpoint, ...),
  * optionally serves a tiny HTTP control endpoint so your phone can send
    commands to the feeder over the LAN (no new hardware, just the laptop).

Usage:
  python3 petwatch_gateway.py --webhook https://.../event
  python3 petwatch_gateway.py --webhook https://.../event --control 8090
  python3 petwatch_gateway.py --control 8090            # events to console only
"""

import argparse
import http.server
import json
import os
import sys
import threading
import time
import urllib.request

PORT = os.environ.get("PETWATCH_SERIAL", "/dev/ttyUSB0")
BAUD = 9600
TOKEN = os.environ.get("PETWATCH_TOKEN", "petwatch")

# Lines that get forwarded to the webhook, by prefix.
FORWARD = ("FEED,", "EATEN,", "ALERT,", "WARN,", "ERR,", "RTC,", "OK,", "PORTION ->")


class SerialLine:
    """Minimal raw serial reader/writer (termios, no pyserial dependency)."""

    def __init__(self, path):
        import termios
        self.path = path
        self.termios = termios
        buf = bytearray()
        self.buf = buf

    def open(self):
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
        a = self.termios.tcgetattr(fd)
        a[0] = 0                              # raw input
        a[1] = 0                              # raw output
        a[3] = 0
        a[2] = self.termios.CS8 | self.termios.CREAD | self.termios.CLOCAL
        a[4] = self.termios.B9600
        a[5] = self.termios.B9600
        a[6][self.termios.VMIN] = 0
        a[6][self.termios.VTIME] = 1
        self.termios.tcsetattr(fd, self.termios.TCSANOW, a)
        self.termios.tcflush(fd, self.termios.TCIOFLUSH)
        self.fd = fd
        self.buf = self.buf[:0]

    def readline(self, timeout=0.2):
        end = time.time() + timeout
        while b"\n" not in self.buf and time.time() < end:
            try:
                c = os.read(self.fd, 256)
            except OSError:
                c = b""
            if c:
                self.buf += c
        if b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            return line.decode("utf-8", "replace").strip()
        return None

    def write(self, text):
        os.write(self.fd, (text + "\n").encode())


def post_webhook(url, line):
    payload = json.dumps({"device": "petwatch", "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                          "event": line}).encode()
    req = urllib.request.Request(url, data=payload,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except Exception as e:
        print(f"[webhook] failed: {e}", flush=True)
        return None


def serial_loop(args):
    ser = SerialLine(args.port)
    while True:
        try:
            ser.open()
            print(f"[gateway] serial open {args.port}", flush=True)
            while True:
                line = ser.readline()
                if not line:
                    continue
                if args.verbose:
                    print(f"[serial] {line}", flush=True)
                if args.webhook and line.startswith(FORWARD):
                    st = post_webhook(args.webhook, line)
                    print(f"[webhook] {st} <- {line}", flush=True)
        except Exception as e:
            print(f"[gateway] serial error: {e}, retrying in 3s", flush=True)
            time.sleep(3)


class Control(http.server.BaseHTTPRequestHandler):
    ser = None

    def do_GET(self):
        path = self.path.split("?")[0]
        parts = path.strip("/").split("/")
        if len(parts) == 3 and parts[0] == "cmd" and parts[1] == TOKEN:
            cmd = parts[2]
            if cmd not in ("feed", "man", "status", "portion", "settime"):
                self.send_text("unknown command", 400)
                return
            self.ser.write(cmd)
            time.sleep(0.6)
            reply = ""
            for _ in range(10):
                line = self.ser.readline(0.2)
                if line:
                    reply += line + "\n"
            self.send_text(reply.strip() or "(no reply)")
        else:
            self.send_text("usage: /cmd/<token>/<feed|man|status>", 404)

    def send_text(self, text, code=200):
        body = (text + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def control_loop(args, ser):
    Control.ser = ser
    srv = http.server.HTTPServer(("0.0.0.0", args.control), Control)
    print(f"[control] http://<this-laptop-ip>:{args.control}/cmd/{TOKEN}/feed", flush=True)
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--webhook", default=os.environ.get("PETWATCH_WEBHOOK"))
    ap.add_argument("--control", type=int, metavar="PORT", help="enable HTTP control endpoint")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    ser = SerialLine(args.port)
    threads = [
        threading.Thread(target=serial_loop, args=(args,), daemon=True),
    ]
    if args.control:
        threads.append(threading.Thread(target=control_loop, args=(args, ser), daemon=True))
    for t in threads:
        t.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print("\n[gateway] bye")
        sys.exit(0)


if __name__ == "__main__":
    main()