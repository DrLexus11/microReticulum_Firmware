#!/usr/bin/env python3
"""Deterministic two-client acceptance probe for an RRC v1 hub."""

import argparse
import json
import os
import sys
import threading
import time

try:
    import RNS
    from nomadnet.vendor import cbor
except ImportError as error:
    sys.exit(
        "error: RNS and NomadNet are required; run this with the RNS virtualenv "
        "(%s)" % error
    )


RRC_VERSION = 1

K_V = 0
K_T = 1
K_ID = 2
K_TS = 3
K_SRC = 4
K_ROOM = 5
K_BODY = 6
K_NICK = 7

T_HELLO = 1
T_WELCOME = 2
T_JOIN = 10
T_JOINED = 11
T_PART = 12
T_PARTED = 13
T_MSG = 20
T_PING = 30
T_PONG = 31
T_ERROR = 40

B_HELLO_NAME = 0
B_HELLO_VER = 1
B_HELLO_CAPS = 2
B_WELCOME_HUB = 0
B_WELCOME_VER = 1
B_WELCOME_CAPS = 2
B_WELCOME_LIMITS = 3

L_MAX_NICK_BYTES = 0
L_MAX_ROOM_NAME_BYTES = 1
L_MAX_MSG_BODY_BYTES = 2
L_MAX_ROOMS_PER_SESSION = 3
L_RATE_LIMIT_MSGS_PER_MINUTE = 4

CAP_RESOURCE_ENVELOPE = 0
CAP_ACTION = 1


class ProbeFailure(RuntimeError):
    pass


def parse_destination_hash(value):
    try:
        result = bytes.fromhex(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("destination hash must be hexadecimal") from error
    if len(result) != RNS.Reticulum.TRUNCATED_HASHLENGTH // 8:
        raise argparse.ArgumentTypeError("destination hash must contain 16 bytes")
    return result


def load_identity(state_dir, name):
    path = os.path.join(state_dir, "%s.identity" % name)
    if os.path.isfile(path):
        identity = RNS.Identity.from_file(path)
        if identity is None:
            raise ProbeFailure("could not load identity %s" % path)
        return identity
    identity = RNS.Identity()
    identity.to_file(path)
    return identity


def envelope(message_type, source, room=None, body=None, nickname=None):
    value = {
        K_V: RRC_VERSION,
        K_T: message_type,
        K_ID: os.urandom(8),
        K_TS: int(time.time() * 1000),
        K_SRC: source,
    }
    if room is not None:
        value[K_ROOM] = room
    if body is not None:
        value[K_BODY] = body
    if nickname:
        value[K_NICK] = nickname
    return value


def member_list_contains(value, identity_hash):
    body = value.get(K_BODY)
    return isinstance(body, list) and any(
        isinstance(member, (bytes, bytearray)) and bytes(member) == identity_hash
        for member in body
    )


class RRCClient:
    def __init__(self, destination, identity, nickname, timeout, link_retries):
        self.destination = destination
        self.identity = identity
        self.nickname = nickname
        self.timeout = timeout
        self.link_retries = link_retries
        self.link = None
        self.welcome = None
        self.received = []
        self.decode_errors = []
        self.condition = threading.Condition()

    def _record(self, value):
        with self.condition:
            self.received.append(value)
            self.condition.notify_all()

    def _on_packet(self, data, packet):
        del packet
        try:
            value = cbor.decode(data)
        except Exception as error:
            with self.condition:
                self.decode_errors.append(str(error))
                self.condition.notify_all()
            return
        if not isinstance(value, dict):
            with self.condition:
                self.decode_errors.append("decoded RRC payload is not a map")
                self.condition.notify_all()
            return

        self._record(value)
        if value.get(K_T) == T_PING:
            try:
                self.send(T_PONG, body=value.get(K_BODY))
            except Exception:
                # The Link may already be closing. The main wait path will
                # report that state if it affects the acceptance operation.
                pass

    def snapshot(self):
        with self.condition:
            return len(self.received)

    def wait_for(self, start, predicate, description, timeout=None, errors=True):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        with self.condition:
            while True:
                for value in self.received[start:]:
                    if errors and value.get(K_T) == T_ERROR:
                        raise ProbeFailure(
                            "%s received RRC ERROR while waiting for %s: %s"
                            % (self.nickname, description, value.get(K_BODY))
                        )
                    if predicate(value):
                        return value
                if self.decode_errors:
                    raise ProbeFailure(
                        "%s received invalid CBOR: %s"
                        % (self.nickname, self.decode_errors[-1])
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self.condition.wait(min(remaining, 0.25))

    def send(self, message_type, room=None, body=None):
        link = self.link
        if link is None or link.status != RNS.Link.ACTIVE:
            raise ProbeFailure("%s has no active Link" % self.nickname)
        value = envelope(
            message_type,
            self.identity.hash,
            room=room,
            body=body,
            nickname=self.nickname if message_type != T_PONG else None,
        )
        payload = cbor.encode(value)
        packet = RNS.Packet(link, payload)
        try:
            packet.pack()
        except Exception as error:
            raise ProbeFailure("%s payload exceeds the Link MDU" % self.nickname) from error
        packet.send()
        return value

    def connect(self):
        self.welcome = None
        last_status = None
        for attempt in range(1, self.link_retries + 1):
            link = RNS.Link(self.destination)
            self.link = link
            link.set_packet_callback(self._on_packet)
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline and link.status == RNS.Link.PENDING:
                time.sleep(0.05)
            last_status = link.status
            if link.status != RNS.Link.ACTIVE:
                try:
                    link.teardown()
                except Exception:
                    pass
                RNS.Transport.request_path(self.destination.hash)
                time.sleep(min(1.0, self.timeout))
                continue

            link.identify(self.identity)
            time.sleep(0.5)
            start = self.snapshot()
            hello_body = {
                B_HELLO_NAME: "rnode-rrc-probe",
                B_HELLO_VER: "1.0",
                B_HELLO_CAPS: {
                    CAP_RESOURCE_ENVELOPE: False,
                    CAP_ACTION: True,
                },
            }
            hello_deadline = time.monotonic() + self.timeout
            while time.monotonic() < hello_deadline:
                self.send(T_HELLO, body=hello_body)
                welcome = self.wait_for(
                    start,
                    lambda value: value.get(K_T) == T_WELCOME,
                    "WELCOME",
                    timeout=min(3.0, max(0.1, hello_deadline - time.monotonic())),
                )
                if welcome is not None:
                    self.welcome = welcome
                    return welcome
                if link.status != RNS.Link.ACTIVE:
                    break

            try:
                link.teardown()
            except Exception:
                pass
            RNS.Transport.request_path(self.destination.hash)
            time.sleep(min(1.0, self.timeout))

        raise ProbeFailure(
            "%s could not establish and welcome a Link after %d attempts "
            "(last status %s)"
            % (self.nickname, self.link_retries, last_status)
        )

    def join(self, room):
        start = self.snapshot()
        self.send(T_JOIN, room=room)
        # The member list in JOINED is optional advisory data (RRCRequirements
        # 7, type 11), so matching on it makes the probe reject conformant
        # hubs that omit it -- rrcd sends JOINED with no body at all, and this
        # predicate reported that as "did not receive JOINED" while the join
        # had in fact succeeded. Match the confirmation; verify the list only
        # when the hub chose to send one.
        joined = self.wait_for(
            start,
            lambda value: value.get(K_T) == T_JOINED
            and value.get(K_ROOM) == room,
            "JOINED for %s" % room,
        )
        if joined is None:
            raise ProbeFailure("%s did not receive JOINED for %s" % (self.nickname, room))
        if joined.get(K_BODY) is not None and not member_list_contains(
            joined, self.identity.hash
        ):
            raise ProbeFailure(
                "%s received JOINED for %s carrying a member list that omits it"
                % (self.nickname, room)
            )
        return joined

    def part(self, room):
        start = self.snapshot()
        self.send(T_PART, room=room)
        # Same optional-advisory member list as JOINED: match the confirmation,
        # check the list only when one was sent.
        parted = self.wait_for(
            start,
            lambda value: value.get(K_T) == T_PARTED
            and value.get(K_ROOM) == room,
            "PARTED for %s" % room,
        )
        if parted is None:
            raise ProbeFailure("%s did not receive PARTED for %s" % (self.nickname, room))
        if parted.get(K_BODY) is not None and not member_list_contains(
            parted, self.identity.hash
        ):
            raise ProbeFailure(
                "%s received PARTED for %s carrying a member list that omits it"
                % (self.nickname, room)
            )
        return parted

    def wait_message(self, start, room, body, source, nickname):
        message = self.wait_for(
            start,
            lambda value: value.get(K_T) == T_MSG
            and value.get(K_ROOM) == room
            and value.get(K_BODY) == body,
            "message %r" % body,
        )
        if message is None:
            raise ProbeFailure("%s did not receive %r" % (self.nickname, body))
        actual_source = message.get(K_SRC)
        if not isinstance(actual_source, (bytes, bytearray)) or bytes(actual_source) != source:
            raise ProbeFailure("%s received %r with incorrect source" % (self.nickname, body))
        if message.get(K_NICK) != nickname:
            raise ProbeFailure("%s received %r with incorrect nickname" % (self.nickname, body))
        return message

    def teardown(self):
        link = self.link
        self.link = None
        if link is not None:
            try:
                link.teardown()
            except Exception:
                pass


def resolve_destination(destination_hash, timeout):
    if not RNS.Transport.has_path(destination_hash):
        RNS.Transport.request_path(destination_hash)
    deadline = time.monotonic() + timeout
    identity = None
    while time.monotonic() < deadline:
        identity = RNS.Identity.recall(destination_hash)
        if RNS.Transport.has_path(destination_hash) and identity is not None:
            break
        time.sleep(0.1)
    if identity is None or not RNS.Transport.has_path(destination_hash):
        raise ProbeFailure("no path or identity for hub %s" % destination_hash.hex())
    destination = RNS.Destination(
        identity,
        RNS.Destination.OUT,
        RNS.Destination.SINGLE,
        "rrc",
        "hub",
    )
    if destination.hash != destination_hash:
        raise ProbeFailure("destination hash does not match rrc.hub")
    return destination


def validate_welcome(value, expected_hub_name):
    body = value.get(K_BODY)
    if not isinstance(body, dict):
        raise ProbeFailure("WELCOME body is not a map")
    hub_name = body.get(B_WELCOME_HUB)
    if expected_hub_name and hub_name != expected_hub_name:
        raise ProbeFailure("unexpected hub name %r" % hub_name)
    limits = body.get(B_WELCOME_LIMITS)
    if not isinstance(limits, dict):
        raise ProbeFailure("WELCOME does not advertise limits")
    required = (
        L_MAX_NICK_BYTES,
        L_MAX_ROOM_NAME_BYTES,
        L_MAX_MSG_BODY_BYTES,
        L_MAX_ROOMS_PER_SESSION,
        L_RATE_LIMIT_MSGS_PER_MINUTE,
    )
    if any(not isinstance(limits.get(key), int) or limits[key] <= 0 for key in required):
        raise ProbeFailure("WELCOME contains invalid limits")
    return {
        "hub": hub_name,
        "version": body.get(B_WELCOME_VER),
        "capabilities": body.get(B_WELCOME_CAPS),
        "limits": limits,
    }


def run_probe(args):
    os.makedirs(args.state_dir, exist_ok=True)
    identity_a = load_identity(args.state_dir, "client-a")
    identity_b = load_identity(args.state_dir, "client-b")

    RNS.loglevel = RNS.LOG_ERROR
    RNS.Reticulum(configdir=args.rns_configdir)
    destination = resolve_destination(args.hub_hash, args.timeout)
    hops = RNS.Transport.hops_to(args.hub_hash)

    client_a = RRCClient(destination, identity_a, args.nick_a, args.timeout, args.link_retries)
    client_b = RRCClient(destination, identity_b, args.nick_b, args.timeout, args.link_retries)
    suffix = os.urandom(3).hex()
    message_a = "%s-A-%s" % (args.message_prefix, suffix)
    message_b = "%s-B-%s" % (args.message_prefix, suffix)
    reconnect_message = "%s-RECONNECT-%s" % (args.message_prefix, suffix)
    result = {
        "hub": args.hub_hash.hex(),
        "hops": hops,
        "room": args.room,
        "identity_a": identity_a.hash.hex(),
        "identity_b": identity_b.hash.hex(),
        "messages": [message_a, message_b, reconnect_message],
    }

    try:
        welcome_a = validate_welcome(client_a.connect(), args.expect_hub_name)
        welcome_b = validate_welcome(client_b.connect(), args.expect_hub_name)
        if welcome_a["limits"] != welcome_b["limits"]:
            raise ProbeFailure("clients received different WELCOME limits")
        result["welcome"] = welcome_a

        client_a.join(args.room)
        client_b.join(args.room)

        start_a = client_a.snapshot()
        start_b = client_b.snapshot()
        client_a.send(T_MSG, room=args.room, body=message_a)
        client_a.wait_message(start_a, args.room, message_a, identity_a.hash, args.nick_a)
        client_b.wait_message(start_b, args.room, message_a, identity_a.hash, args.nick_a)

        start_a = client_a.snapshot()
        start_b = client_b.snapshot()
        client_b.send(T_MSG, room=args.room, body=message_b)
        client_a.wait_message(start_a, args.room, message_b, identity_b.hash, args.nick_b)
        client_b.wait_message(start_b, args.room, message_b, identity_b.hash, args.nick_b)

        # Abruptly close B and require A to observe the membership cleanup.
        start_a = client_a.snapshot()
        client_b.teardown()
        departed = client_a.wait_for(
            start_a,
            lambda value: value.get(K_T) == T_PARTED
            and value.get(K_ROOM) == args.room
            and member_list_contains(value, identity_b.hash),
            "B disconnect notification",
        )
        if departed is None and args.require_notifications:
            raise ProbeFailure("A did not observe B's disconnect cleanup")

        validate_welcome(client_b.connect(), args.expect_hub_name)
        client_b.join(args.room)
        start_a = client_a.snapshot()
        start_b = client_b.snapshot()
        client_b.send(T_MSG, room=args.room, body=reconnect_message)
        client_a.wait_message(
            start_a, args.room, reconnect_message, identity_b.hash, args.nick_b
        )
        client_b.wait_message(
            start_b, args.room, reconnect_message, identity_b.hash, args.nick_b
        )

        client_b.part(args.room)
        client_a.part(args.room)
        result["status"] = "PASS"
        return result
    finally:
        client_b.teardown()
        client_a.teardown()


def build_parser():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_state = os.path.abspath(
        os.path.join(script_dir, "..", "..", "state", "rrc-probe")
    )
    parser = argparse.ArgumentParser(
        description=(
            "Run deterministic two-client message, attribution, disconnect and "
            "reconnect acceptance against an RRC v1 hub"
        )
    )
    parser.add_argument("hub_hash", type=parse_destination_hash)
    parser.add_argument("--room", default="#rad01-probe")
    parser.add_argument("--nick-a", default="probe-a")
    parser.add_argument("--nick-b", default="probe-b")
    parser.add_argument("--message-prefix", default="RRC-PROBE")
    parser.add_argument("--expect-hub-name", default=None)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--link-retries", type=int, default=3)
    parser.add_argument("--state-dir", default=default_state)
    parser.add_argument(
        "--rns-configdir",
        default=os.environ.get("RNS_CONFIGDIR"),
        help="Reticulum config directory (default: RNS_CONFIGDIR or normal user config)",
    )
    # PARTED on a peer's disconnect is a notification the spec makes optional
    # ("may notify existing members", RRCRequirements 7). Our firmware sends
    # it and its acceptance depends on it, so this stays on by default; turn
    # it off to run the probe against a conformant hub that omits it, such as
    # rrcd.
    parser.add_argument("--no-require-notifications", dest="require_notifications",
                        action="store_false", default=True,
                        help="tolerate a hub that does not notify members of "
                             "joins and disconnects")
    parser.add_argument("--json", action="store_true")
    return parser


def main():
    args = build_parser().parse_args()
    if not args.room.strip():
        sys.exit("error: room must not be empty")
    args.room = args.room.strip().lower()
    if args.timeout <= 0:
        sys.exit("error: --timeout must be positive")
    if args.link_retries <= 0:
        sys.exit("error: --link-retries must be positive")
    try:
        result = run_probe(args)
    except (ProbeFailure, OSError, ValueError) as error:
        if args.json:
            print(json.dumps({"status": "FAIL", "error": str(error)}, indent=2))
        else:
            print("FAIL: %s" % error)
        return 1

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("hub: %s (%s hop(s))" % (result["hub"], result["hops"]))
        print("room: %s" % result["room"])
        print("A: %s" % result["identity_a"])
        print("B: %s" % result["identity_b"])
        for message in result["messages"]:
            print("verified: %s" % message)
        print("WELCOME limits: %s" % result["welcome"]["limits"])
        print("PASS: bidirectional fanout, attribution and reconnect cleanup")
    return 0


if __name__ == "__main__":
    sys.exit(main())
