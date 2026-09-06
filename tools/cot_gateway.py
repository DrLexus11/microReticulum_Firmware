#!/usr/bin/env python3
"""Turn compact position reports from the mesh into a CoT feed for ATAK.

TAKCapability.md §7 step 4, and the piece that makes the rest of it useful:
ATAK never speaks Reticulum. It expects an IP network with a feed of Cursor on
Target, so this sits where the two meet -- a Reticulum destination on one side,
an ordinary CoT stream on the other. No plugin, no fork, no client work.

    python tools/cot_gateway.py --identity ~/.rns_gateway_identity

It prints its destination hash on startup. Provision that onto each node (or
paste it into Columba's Position Reporting card) and reports start arriving.

Three outputs, and which you want depends on whether a TAK server is in the
picture.

  * **--forward host:port** sends each event to a TAK server as a CoT producer.
    This is the right shape when one exists: OpenTAKServer takes CoT on UDP
    8087, and then it -- not this -- serves EUDs on 8088/8089 and draws its own
    map. Two things both trying to be the server is the arrangement to avoid.

        opentakserver &
        python tools/cot_gateway.py --forward 127.0.0.1:8087 --no-tcp

  * **TCP** on 8087, for ATAK connecting straight to this with no server at all.
    Every connected client gets every event. Fine for a bench check; note it
    collides conceptually, though not on the wire, with OTS's UDP 8087.
  * **UDP multicast** to 239.2.3.1:6969, where ATAK picks up situational
    awareness on a local network with no configuration whatsoever.

Why the mesh side is twenty bytes and this side is seven hundred: a CoT event
is XML, and XML on a LoRa radio is sixty-seven reports an hour across the whole
channel. Compact on the wire, expanded here where bandwidth is free. See
tools/position_codec.py for the format.
"""

import argparse
import os
import socket
import struct
import sys
import threading
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import position_codec  # noqa: E402

try:
    import RNS
except ImportError:
    RNS = None

APP_NAME = "rnstransport"
ASPECTS = ("position", "report")

# CoT's own "I do not know" value. Not zero: zero metres is sea level and zero
# error is a claim no receiver can make, and ATAK renders both as fact. The
# firmware flags an unknown altitude rather than sending sea level for the same
# reason; this is where that care has to survive the format change.
COT_UNKNOWN = 9999999.0

# A friendly unit, unaffiliated. Operators retype this constantly; it is a
# default rather than a decision.
DEFAULT_COT_TYPE = "a-f-G-U-C"

DEFAULT_TCP_PORT = 8087
DEFAULT_MULTICAST_GROUP = "239.2.3.1"
DEFAULT_MULTICAST_PORT = 6969


def parse_host_port(text):
    """"host:port" for --forward. Raises rather than guessing a port: sending
    CoT somewhere nobody is listening looks identical to sending nothing."""
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit():
        raise SystemExit("--forward wants HOST:PORT, got %r" % text)
    return (host, int(port))


def cot_time(when):
    """CoT wants ISO 8601 in UTC with a Z, to millisecond precision."""
    return when.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def build_cot(fix, uid, callsign, stale_seconds, received_at=None):
    """One CoT event from one decoded report.

    `stale` is the field that matters most here. It is what makes ATAK drop a
    track it can no longer trust, so it has to follow the reporting cadence: too
    short and every marker flickers out between reports, too long and a node
    that went off the air an hour ago is still on the map looking current.
    """
    received_at = received_at or datetime.now(timezone.utc)

    # Prefer the time the fix was taken. A node with no clock sends zero, and
    # then the only honest stamp available is when we received it -- which is
    # later than the truth, never earlier, so a marker errs toward looking
    # older rather than fresher than it is.
    if fix.fix_unix_s:
        taken = datetime.fromtimestamp(fix.fix_unix_s, tz=timezone.utc)
    else:
        taken = received_at

    event = ET.Element("event", {
        "version": "2.0",
        "uid": uid,
        "type": DEFAULT_COT_TYPE,
        "how": "m-g",              # measured, GPS
        "time": cot_time(received_at),
        "start": cot_time(taken),
        "stale": cot_time(received_at + timedelta(seconds=stale_seconds)),
    })

    ET.SubElement(event, "point", {
        "lat": "%.7f" % (fix.lat_e7 / 1e7),
        "lon": "%.7f" % (fix.lon_e7 / 1e7),
        "hae": "%.1f" % (float(fix.alt_m) if fix.alt_known else COT_UNKNOWN),
        # Circular error. 255 on the wire means "worse than 254 m" rather than
        # exactly 255, but CoT has no way to say that, so the number is passed
        # through and the operator sees a large error rather than a precise one.
        "ce": "%.1f" % (float(fix.accuracy_m) if fix.accuracy_m else COT_UNKNOWN),
        # Linear (vertical) error is never measured on this path.
        "le": "%.1f" % COT_UNKNOWN,
    })

    detail = ET.SubElement(event, "detail")
    ET.SubElement(detail, "contact", {"callsign": callsign})
    ET.SubElement(detail, "__group", {"name": "Cyan", "role": "Team Member"})
    if fix.alt_known:
        ET.SubElement(detail, "precisionlocation", {"altsrc": "GPS"})
    # Track goes in only when there is something to say. An absent track is
    # read as "not reported"; a track of zero is read as stationary and facing
    # north, which is a different claim.
    if fix.course_known or fix.speed_cms:
        track = {}
        if fix.speed_cms:
            track["speed"] = "%.2f" % (fix.speed_cms / 100.0)
        if fix.course_known:
            track["course"] = "%.1f" % (fix.course_ddeg / 10.0)
        ET.SubElement(detail, "track", track)
    if fix.sats:
        ET.SubElement(detail, "uid", {"Droid": callsign})

    return b'<?xml version="1.0" standalone="yes"?>' + ET.tostring(event, encoding="utf-8")


class CotFanout:
    """Everything that wants CoT, fed from one place.

    TCP clients come and go and a dead one must not take the gateway with it,
    so a failed write costs that client and nothing else.
    """

    def __init__(self, tcp_port=None, multicast=None, forward=(), verbose=False):
        self.verbose = verbose
        self._clients = []
        self._lock = threading.Lock()
        self._multicast = multicast
        # Unicast CoT to a TAK server. Separate from the multicast socket
        # because they are different intents: one publishes to whoever is
        # listening on a LAN, the other hands events to a server that owns
        # distribution from there.
        self._forward = list(forward)
        self._udp = None
        if multicast or self._forward:
            self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        if tcp_port:
            thread = threading.Thread(target=self._accept_loop, args=(tcp_port,),
                                      daemon=True)
            thread.start()

    def _accept_loop(self, port):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", port))
        server.listen(8)
        print("[cot] TCP listening on 0.0.0.0:%d" % port)
        while True:
            try:
                client, address = server.accept()
            except OSError:
                break
            print("[cot] client connected from %s:%d" % address)
            with self._lock:
                self._clients.append(client)

    def send(self, payload):
        if self._udp is not None and self._multicast:
            try:
                self._udp.sendto(payload, self._multicast)
            except OSError as error:
                print("[cot] multicast send failed: %s" % error)

        for target in self._forward:
            try:
                self._udp.sendto(payload, target)
            except OSError as error:
                # A server that is down must not stop the others, and must not
                # stop the gateway: reports keep arriving from the mesh either
                # way, and the log is where an operator finds out.
                print("[cot] forward to %s:%d failed: %s" % (target + (error,)))

        with self._lock:
            clients = list(self._clients)
        for client in clients:
            try:
                client.sendall(payload + b"\n")
            except OSError:
                # Gone. Drop it and keep serving everyone else.
                with self._lock:
                    if client in self._clients:
                        self._clients.remove(client)
                try:
                    client.close()
                except OSError:
                    pass
                print("[cot] client disconnected")


class PositionGateway:
    def __init__(self, args):
        self.args = args
        self.fanout = CotFanout(
            tcp_port=None if args.no_tcp else args.tcp_port,
            multicast=None if args.no_multicast else (args.multicast_group,
                                                      args.multicast_port),
            forward=[parse_host_port(target) for target in args.forward],
            verbose=args.verbose,
        )
        self.seen = {}

    def start(self):
        RNS.Reticulum(self.args.config)
        identity = self._load_identity()
        self.destination = RNS.Destination(
            identity,
            RNS.Destination.IN,
            RNS.Destination.SINGLE,
            APP_NAME,
            *ASPECTS,
        )
        self.destination.set_packet_callback(self._on_packet)
        # Announce so nodes can learn a path here. §3 of the capability note
        # turns on this being cheap: the gateway is stationary and announces on
        # an ordinary schedule, and the nodes never announce to carry position.
        self.destination.announce()
        print("[gateway] destination %s" % RNS.prettyhexrep(self.destination.hash))
        print("[gateway] provision that hash onto each node, or paste it into")
        print("[gateway] Columba's Position Reporting card.")

        while True:
            time.sleep(self.args.announce_interval)
            self.destination.announce()

    def _load_identity(self):
        path = os.path.expanduser(self.args.identity)
        if os.path.exists(path):
            return RNS.Identity.from_file(path)
        identity = RNS.Identity()
        identity.to_file(path)
        # The destination hash is derived from this key. Losing the file means
        # every node has to be reprovisioned, so it is created 0600 and said
        # out loud rather than left to be discovered.
        os.chmod(path, 0o600)
        print("[gateway] created a new identity at %s -- keep it" % path)
        return identity

    def _on_packet(self, data, packet):
        fix = position_codec.decode(bytes(data))
        if fix is None:
            print("[gateway] undecodable report, %d bytes" % len(data))
            return

        # One track per sender, from the sender_id carried in the report.
        #
        # This used to derive the uid from the packet hash, which is different
        # for every packet -- so every report became a *new* track and a map
        # filled with one person's ghosts. Two live reports from one phone on
        # 2026-09-06 arrived as RAD-13aaef and RAD-f80e9a, and that is what put
        # sender_id on the wire in version 2.
        #
        # A Reticulum packet to a SINGLE destination is anonymous by
        # construction, so the identifier has to travel inside the payload;
        # there is nothing on the packet to fall back to that would be stable.
        uid = "urtn-%08x" % fix.sender_id
        callsign = "%s%06X" % (self.args.callsign_prefix, fix.sender_id & 0xFFFFFF)

        payload = build_cot(fix, uid, callsign, self.args.stale_seconds)
        self.fanout.send(payload)

        first = uid not in self.seen
        self.seen[uid] = time.time()
        print("[gateway] %s %s %.6f,%.6f%s%s" % (
            "new " if first else "move", callsign,
            fix.lat_e7 / 1e7, fix.lon_e7 / 1e7,
            " +/-%dm" % fix.accuracy_m if fix.accuracy_m else "",
            " alt %dm" % fix.alt_m if fix.alt_known else "",
        ))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--identity", default="~/.rns_cot_gateway_identity",
                        help="where this gateway's key lives (created if absent)")
    parser.add_argument("--config", default=None,
                        help="Reticulum config directory")
    parser.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    parser.add_argument("--no-tcp", action="store_true")
    parser.add_argument("--multicast-group", default=DEFAULT_MULTICAST_GROUP)
    parser.add_argument("--multicast-port", type=int, default=DEFAULT_MULTICAST_PORT)
    parser.add_argument("--no-multicast", action="store_true")
    parser.add_argument("--forward", action="append", default=[],
                        metavar="HOST:PORT",
                        help="send CoT to a TAK server as a producer, repeatable; "
                             "OpenTAKServer listens on UDP 8087")
    parser.add_argument("--callsign-prefix", default="RAD-")
    # Long enough that a missed report does not blink the marker off the map,
    # short enough that a node which stopped reporting stops being believed.
    # Three reporting intervals at the one-a-minute default.
    parser.add_argument("--stale-seconds", type=int, default=180)
    parser.add_argument("--announce-interval", type=int, default=300,
                        help="seconds between announces, so nodes keep a path")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    # This is a daemon, and a daemon's output is always redirected somewhere.
    # Python block-buffers a redirected stdout, so without this the startup
    # banner -- including the destination hash nobody can proceed without --
    # sits invisible in a buffer until the process exits or 8 KB accumulate.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    if RNS is None:
        raise SystemExit("RNS is not installed in this interpreter.\n"
                         "Try: pip install rns")

    PositionGateway(args).start()


if __name__ == "__main__":
    main()
