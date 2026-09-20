#!/usr/bin/env python3
"""Measure chat arrival between two running CoT endpoints, over their mesh.

Example with the phone's local endpoint exposed by `adb forward tcp:19087
tcp:18087` and the host bridge serving Waydroid:

    python3 tools/tak_chat_soak.py --left-host 192.168.240.1 \
        --left-uid urtn-<deck hash> --right-uid urtn-<phone hash>

Both sockets remain open to observe arrival, not just sender-side proofs.
No self-report is injected, so the probe does not replace ATAK's callsign or
position. Each line of output is JSON; retain it alongside transport logs.
"""

import argparse
import json
import socket
import statistics
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

from cot_endpoint import CotStream
from tak_send_dm import build


def chat_id(event):
    try:
        root = ET.fromstring(event)
    except ET.ParseError:
        return None
    if root.get("type") != "b-t-f":
        return None
    chat = root.find("./detail/__chat")
    return chat.get("messageId") if chat is not None else None


def positive(value):
    value = float(value)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def emit(value):
    print(json.dumps(value, sort_keys=True), flush=True)


class Endpoint:
    def __init__(self, name, address):
        self.name = name
        self.socket = socket.create_connection(address, timeout=5)
        self.socket.settimeout(0.5)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.condition = threading.Condition()
        self.arrivals = {}
        self.pending = set()
        self.error = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        stream = CotStream()
        try:
            while not self.stopped.is_set():
                try:
                    chunk = self.socket.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise ConnectionError("endpoint disconnected")
                for event in stream.feed(chunk):
                    message_id = chat_id(event)
                    with self.condition:
                        if message_id in self.pending:
                            self.arrivals.setdefault(message_id, time.monotonic())
                            self.condition.notify_all()
        except OSError as error:
            with self.condition:
                self.error = str(error)
                self.condition.notify_all()

    def expect(self, message_id):
        with self.condition:
            self.pending.add(message_id)

    def wait(self, message_id, timeout):
        with self.condition:
            self.condition.wait_for(
                lambda: message_id in self.arrivals or self.error, timeout)
            self.pending.discard(message_id)
            if self.error:
                raise ConnectionError("%s: %s" % (self.name, self.error))
            return self.arrivals.pop(message_id, None)

    def close(self):
        self.stopped.set()
        self.socket.close()
        self.thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--left-host", default="127.0.0.1")
    parser.add_argument("--left-port", type=int, default=18087)
    parser.add_argument("--right-host", default="127.0.0.1")
    parser.add_argument("--right-port", type=int, default=19087)
    parser.add_argument("--left-uid", required=True)
    parser.add_argument("--right-uid", required=True)
    parser.add_argument("--count", type=int, default=10,
                        help="total messages, alternating directions")
    parser.add_argument("--timeout", type=positive, default=120)
    parser.add_argument("--interval", type=positive, default=5,
                        help="quiet seconds after each arrival or timeout")
    parser.add_argument("--direction", choices=("both", "left", "right"), default="both")
    parser.add_argument("--text", default="latency probe")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be positive")
    for uid in (args.left_uid, args.right_uid):
        try:
            valid = uid.startswith("urtn-") and len(bytes.fromhex(uid[5:])) == 16
        except ValueError:
            valid = False
        if not valid:
            parser.error("UIDs must be urtn- followed by a 16-byte hex hash")

    endpoints = []
    rows = []
    try:
        endpoints.append(Endpoint("left", (args.left_host, args.left_port)))
        endpoints.append(Endpoint("right", (args.right_host, args.right_port)))
        uids = (args.left_uid, args.right_uid)
        for index in range(args.count):
            side = index % 2 if args.direction == "both" else int(args.direction == "right")
            sender, receiver = endpoints[side], endpoints[1 - side]
            message_id = str(uuid.uuid4())
            payload = build("ANDROID-LATENCY-PROBE", uids[1 - side],
                            "LATENCY-PROBE", "PEER", args.text, message_id,
                            datetime.now(timezone.utc)).encode("utf-8")
            receiver.expect(message_id)
            started = time.monotonic()
            row = dict(sample=index + 1, message_id=message_id,
                       direction=sender.name + "->" + receiver.name,
                       sent_unix=time.time(), cot_bytes=len(payload))
            emit(dict(event="send", **row))
            sender.socket.sendall(payload)
            arrived = receiver.wait(message_id, args.timeout)
            row["seconds"] = round(arrived - started, 3) if arrived is not None else None
            rows.append(row)
            emit(dict(event="arrival" if arrived is not None else "timeout", **row))
            if index + 1 < args.count:
                time.sleep(args.interval)
    finally:
        for endpoint in endpoints:
            endpoint.close()

    for direction in sorted({row["direction"] for row in rows}):
        samples = [row for row in rows if row["direction"] == direction]
        times = sorted(row["seconds"] for row in samples if row["seconds"] is not None)
        emit(dict(event="summary", direction=direction, sent=len(samples),
                  received=len(times), minimum=min(times) if times else None,
                  median=statistics.median(times) if times else None,
                  maximum=max(times) if times else None))
    return int(any(row["seconds"] is None for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
