#!/usr/bin/env python3
"""Local command-post task inbox and signed Reticulum downlink.

Firehose input is staged, never signed automatically. Use `approve` to authorize
an exact candidate for a pinned peer, or `goto` to issue a local operator task.
All commands share the same private SQLite database and signing identity.
"""

import argparse
import hashlib
import json
import math
import os
import sqlite3
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from contextlib import contextmanager
from pathlib import Path

import task_codec as codec

MAX_COT = 16384
MAX_PENDING = 128
RETRY_SECONDS = 60
# No fixed attempt cap: a task is retried until it expires. Three attempts
# spanned two minutes, so a recipient who walked through a dead spot missed the
# task permanently and nothing redelivered when they came back -- while the task
# itself was still valid for another thirteen minutes. Expiry is the bound that
# means something, and it is already enforced. A 323-byte downlink is 245 ms, so
# a 15-minute task costs at most ~3.7 s of airtime spread across those minutes,
# and a verified receipt stops it at once.
STATES = {codec.RECEIVED: "received", codec.ACCEPTED: "accepted", codec.DECLINED: "declined"}


@contextmanager
def connect(path):
    path = Path(path).expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    # Create privately before SQLite opens it, including under permissive umask.
    fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o600)
    os.close(fd)
    os.chmod(path, 0o600)
    db = sqlite3.connect(path, timeout=10)
    db.row_factory = sqlite3.Row
    db.executescript("""
        CREATE TABLE IF NOT EXISTS peers(name TEXT PRIMARY KEY, public_key BLOB NOT NULL);
        CREATE TABLE IF NOT EXISTS candidates(
          id TEXT PRIMARY KEY, created INTEGER NOT NULL, source TEXT NOT NULL,
          cot_type TEXT NOT NULL, lat INTEGER NOT NULL, lon INTEGER NOT NULL,
          instruction TEXT NOT NULL, target_hint TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS approved_imports(id TEXT PRIMARY KEY, expires INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS tasks(
          id TEXT PRIMARY KEY, created INTEGER NOT NULL, expires INTEGER NOT NULL,
          peer TEXT NOT NULL, public_key BLOB NOT NULL, destination BLOB NOT NULL,
          wire BLOB NOT NULL, state TEXT NOT NULL DEFAULT 'queued',
          attempts INTEGER NOT NULL DEFAULT 0, last_attempt INTEGER NOT NULL DEFAULT 0,
          error TEXT NOT NULL DEFAULT '');
    """)
    try:
        yield db
    finally:
        db.close()


def stage(db, envelope, now):
    """Store an untrusted task candidate. Claimed UID and target are hints only."""
    if len(envelope) > MAX_COT:
        raise ValueError("CoT envelope too large")
    obj = json.loads(envelope)
    xml = obj["cot"]
    if not isinstance(xml, str) or "<!" in xml:
        raise ValueError("XML declarations/entities are not accepted")
    event = ET.fromstring(xml)
    cot_type = event.get("type", "")
    if event.tag != "event" or not cot_type.startswith("t-"):
        return None
    point = event.find("point")
    if point is None:
        raise ValueError("task has no point")
    lat, lon = float(point.attrib["lat"]), float(point.attrib["lon"])
    if not math.isfinite(lat) or not math.isfinite(lon) or not -90 <= lat <= 90 or not -180 <= lon <= 180:
        raise ValueError("invalid task point")
    remarks = event.find("detail/remarks")
    instruction = "" if remarks is None else "".join(remarks.itertext())
    targets = [dest.attrib for dest in event.findall("detail/marti/dest")]
    hints = {name: event.get(name, "")[:128] for name in ("uid", "time", "start", "stale")}
    hints["destinations"] = targets
    key = hashlib.sha256(envelope).hexdigest()[:32]
    with db:
        db.execute("DELETE FROM candidates WHERE created < ?", (now - 3600,))
        db.execute("DELETE FROM approved_imports WHERE expires <= ?", (now,))
        if db.execute("SELECT 1 FROM approved_imports WHERE id=?", (key,)).fetchone():
            return None
        if db.execute("SELECT 1 FROM candidates WHERE id=?", (key,)).fetchone():
            return key
        if db.execute("SELECT count(*) FROM candidates").fetchone()[0] >= MAX_PENDING:
            raise ValueError("candidate inbox is full")
        db.execute("INSERT OR IGNORE INTO candidates VALUES(?,?,?,?,?,?,?,?)",
                   (key, now, str(obj.get("uid", ""))[:128], cot_type[:128],
                    round(lat * 1e7), round(lon * 1e7), instruction[:1024], json.dumps(hints)[:2048]))
    return key


def public_identity(key):
    import RNS
    if len(key) != 64:
        raise ValueError("Reticulum public key must be 64 bytes")
    identity = RNS.Identity(create_keys=False)
    identity.load_public_key(key)
    return identity


def destination_hash(identity):
    import RNS
    return RNS.Destination.hash(identity, codec.APP, *codec.ASPECTS)


def enqueue(db, identity, peer, lat, lon, instruction, lifetime, now, candidate=None):
    row = db.execute("SELECT public_key FROM peers WHERE name=?", (peer,)).fetchone()
    if row is None:
        raise ValueError("unknown peer; pin its public key with peer first")
    remote = public_identity(bytes(row[0]))
    message = codec.Message(codec.GOTO, identity.hash, destination_hash(remote),
                            uuid.uuid4().bytes, now, now + lifetime, lat, lon, instruction)
    wire = codec.encode(message, identity)
    with db:
        db.execute("DELETE FROM tasks WHERE expires < ?", (now - 86400,))
        if db.execute("SELECT count(*) FROM tasks WHERE expires>?", (now,)).fetchone()[0] >= MAX_PENDING:
            raise ValueError("active task limit reached")
        db.execute("INSERT INTO tasks(id,created,expires,peer,public_key,destination,wire) VALUES(?,?,?,?,?,?,?)",
                   (message.task_id.hex(), now, message.expires, peer, row[0], message.recipient, wire))
        if candidate:
            if db.execute("DELETE FROM candidates WHERE id=?", (candidate,)).rowcount != 1:
                raise ValueError("candidate has already been approved or removed")
            db.execute("INSERT INTO approved_imports VALUES(?,?)", (candidate, now + 3600))
    return message.task_id.hex()


def accept_status(db, data, local_destination, now):
    if not codec.HEADER.size + 65 <= len(data) <= codec.MAX_PACKET:
        raise ValueError("invalid acknowledgment size")
    task_id = codec.HEADER.unpack_from(data)[4].hex()
    row = db.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
    if row is None:
        raise ValueError("unknown task")
    message = codec.verify(data, public_identity(bytes(row["public_key"])), local_destination, now)
    if message.kind != codec.STATUS or message.expires != row["expires"] or message.issued < row["created"]:
        raise ValueError("acknowledgment does not match task")
    state = STATES[message.status]
    # A delayed receipt must not undo a human decision. First terminal decision wins.
    with db:
        db.execute("UPDATE tasks SET state=?, error='' WHERE id=? AND state NOT IN ('accepted','declined')",
                   (state, task_id))
    return task_id, db.execute("SELECT state FROM tasks WHERE id=?", (task_id,)).fetchone()[0]


def consume_firehose(path, url, stop):
    import pika
    while not stop.is_set():
        connection = None
        try:
            params = pika.URLParameters(url)
            params.socket_timeout = 5
            params.blocked_connection_timeout = 5
            connection = pika.BlockingConnection(params)
            channel = connection.channel()
            channel.exchange_declare(exchange="firehose", exchange_type="fanout", durable=True, passive=True)
            queue = channel.queue_declare(queue="", exclusive=True, arguments={
                "x-max-length": MAX_PENDING, "x-message-ttl": 60000,
                "x-max-length-bytes": MAX_PENDING * MAX_COT,
            }).method.queue
            channel.queue_bind(queue=queue, exchange="firehose")
            channel.basic_qos(prefetch_count=1)

            def received(ch, method, properties, body):
                try:
                    with connect(path) as db:
                        key = stage(db, body, int(time.time()))
                    if key:
                        print("[task] staged untrusted candidate", key, flush=True)
                except (ValueError, KeyError, TypeError, ET.ParseError, sqlite3.Error):
                    print("[task] rejected malformed/full-inbox firehose input", flush=True)
                finally:
                    ch.basic_ack(method.delivery_tag)

            channel.basic_consume(queue=queue, on_message_callback=received)
            while not stop.is_set():
                connection.process_data_events(time_limit=1)
        except Exception as error:
            # Do not log connection URLs or broker credentials from exceptions.
            print("[task] firehose unavailable:", type(error).__name__, flush=True)
            stop.wait(5)
        finally:
            if connection and connection.is_open:
                connection.close()


def serve(args, identity):
    import RNS
    RNS.Reticulum(args.config)
    destination = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE,
                                  codec.APP, *codec.ASPECTS)

    def incoming(data, packet):
        try:
            with connect(args.db) as db:
                task_id, state = accept_status(db, bytes(data), destination.hash, int(time.time()))
            print("[task]", task_id, state, flush=True)
        except (ValueError, sqlite3.Error):
            print("[task] rejected acknowledgment", flush=True)

    destination.set_packet_callback(incoming)
    print("[task] authority public key", identity.get_public_key().hex(), flush=True)
    print("[task] acknowledgment destination", destination.hash.hex(), flush=True)
    stop = threading.Event()
    worker = None
    if args.firehose:
        # Import synchronously: a missing dependency must fail startup visibly.
        import pika  # noqa: F401
        worker = threading.Thread(target=consume_firehose, args=(args.db, os.environ.get(
            "TAK_AMQP_URL", "amqp://guest:guest@127.0.0.1:5672/%2F"), stop), daemon=True)
        worker.start()
    last_announce = 0
    try:
        while True:
            now = int(time.time())
            if now - last_announce >= 300:
                destination.announce()
                last_announce = now
            with connect(args.db) as db:
                rows = db.execute("SELECT * FROM tasks WHERE expires>? AND state='queued' AND last_attempt<=?",
                                  (now, now - RETRY_SECONDS)).fetchall()
                for row in rows:
                    # Persist before sending so a process crash cannot reset the retry budget.
                    with db:
                        claimed = db.execute("UPDATE tasks SET attempts=attempts+1,last_attempt=? WHERE id=? AND attempts=? AND last_attempt=? AND state='queued'",
                                             (now, row["id"], row["attempts"], row["last_attempt"])).rowcount
                    if not claimed:
                        continue
                    target = bytes(row["destination"])
                    if not RNS.Transport.has_path(target):
                        RNS.Transport.request_path(target)
                        error = "no path; requested discovery"
                    else:
                        remote = RNS.Destination(public_identity(bytes(row["public_key"])),
                                                 RNS.Destination.OUT, RNS.Destination.SINGLE,
                                                 codec.APP, *codec.ASPECTS)
                        try:
                            receipt = RNS.Packet(remote, bytes(row["wire"])).send()
                            error = "awaiting verified receipt" if receipt else "transport rejected packet"
                        except Exception:
                            error = "transport send failed"
                    with db:
                        db.execute("UPDATE tasks SET error=? WHERE id=?", (error, row["id"]))
                    print("[task]", row["id"], error, flush=True)
            time.sleep(1)
    finally:
        stop.set()
        if worker:
            worker.join(timeout=6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default="~/.impr-tak/tasks.sqlite3")
    parser.add_argument("--identity", default="~/.impr-tak/task-authority")
    parser.add_argument("--config", default=None)
    sub = parser.add_subparsers(dest="command", required=True)
    server = sub.add_parser("serve")
    server.add_argument("--firehose", action="store_true")
    sub.add_parser("list")
    peer = sub.add_parser("peer")
    peer.add_argument("name")
    peer.add_argument("public_key", help="128 hex characters, copied from the receiving phone")
    stage_cmd = sub.add_parser("stage")
    stage_cmd.add_argument("file", help="OTS firehose JSON envelope")
    approve = sub.add_parser("approve", help="authorize a staged point as a go-to task")
    approve.add_argument("candidate")
    goto = sub.add_parser("goto", help="issue a go-to task as the local operator")
    goto.add_argument("latitude", type=float)
    goto.add_argument("longitude", type=float)
    for command in (approve, goto):
        command.add_argument("--peer", required=True)
        command.add_argument("--instruction", required=True, help="exact instruction, 1..64 UTF-8 bytes")
        command.add_argument("--lifetime", type=int, default=900)
    args = parser.parse_args()
    args.db = os.path.expanduser(args.db)
    try:
        with connect(args.db) as db:
            now = int(time.time())
            if args.command == "list":
                for row in db.execute("SELECT * FROM candidates ORDER BY created"):
                    print("UNTRUSTED", json.dumps(dict(row), ensure_ascii=True))
                for row in db.execute("SELECT id,peer,created,expires,state,attempts,error FROM tasks ORDER BY created DESC LIMIT 128"):
                    result = dict(row)
                    if now >= row["expires"] and row["state"] not in ("accepted", "declined"):
                        result["state"] = "expired"
                    print("TASK", json.dumps(result))
                return
            if args.command == "peer":
                key = bytes.fromhex(args.public_key)
                remote = public_identity(key)
                with db:
                    db.execute("INSERT OR REPLACE INTO peers VALUES(?,?)", (args.name, key))
                print("Pinned", args.name, "destination", destination_hash(remote).hex())
                return
            if args.command == "stage":
                with open(args.file, "rb") as source:
                    print(stage(db, source.read(MAX_COT + 1), now))
                return
            from time_authority import load_identity
            key_path = os.path.expanduser(args.identity)
            if args.command != "serve" and not os.path.isfile(key_path):
                raise ValueError("start serve first to initialize the task authority")
            identity, _ = load_identity(key_path)
            if args.command == "serve":
                serve(args, identity)
                return
            if args.command == "approve":
                row = db.execute("SELECT * FROM candidates WHERE id=?", (args.candidate,)).fetchone()
                if row is None or now - row["created"] >= 3600:
                    raise ValueError("candidate absent or too old; import a fresh task")
                lat, lon = row["lat"], row["lon"]
            else:
                lat, lon = round(args.latitude * 1e7), round(args.longitude * 1e7)
            print(enqueue(db, identity, args.peer, lat, lon, args.instruction,
                          args.lifetime, now, getattr(args, "candidate", None)))
    except (ValueError, OSError, sqlite3.Error, KeyError, ET.ParseError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
