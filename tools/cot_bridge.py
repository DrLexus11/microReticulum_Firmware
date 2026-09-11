#!/usr/bin/env python3
"""Run a local CoT endpoint: ATAK on 127.0.0.1, a team on the mesh.

    tools/cot_bridge.py --team Cyan --config ~/.impr-tak/bridge-rns

The bridge owns its own Reticulum instance and refuses to run as a client of a
shared one; see tools/cot_bridge.example.conf for the configuration and the
reason. The fleet secret comes from the environment or ~/.impr-tak/fleet-secret,
never from the command line.

ATAK connects to 127.0.0.1:8087 and never to anybody's address. Everything it
sends goes to the team's GROUP destination; everything the team sends is
written back to every connected client. See docs/TAKNative.md.
"""

import argparse
import os
import socket
import stat
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cot_tier2 as tier2
import tak_groups as groups
import tak_identity as tak_identity
import tak_membership as membership
from cot_endpoint import CotOutbound, CotStream

DEFAULT_PORT = 8087
# This node's own Reticulum identity, kept beside the fleet secret. Created on
# first run; losing it changes this node's UID, which to every peer reads as a
# different responder rather than the same one returning.
DEFAULT_IDENTITY_PATH = "~/.impr-tak/node-identity"
# How long a single write to one ATAK client may take before that client is
# treated as gone. Generous for a loopback socket carrying a few hundred bytes,
# and short enough that one stalled client cannot hold up the mesh callback
# thread that every other client's data arrives on.
CLIENT_WRITE_TIMEOUT = 2.0
# How often this node re-announces its membership.
#
# Announces are deliberately expensive in Reticulum -- they are how the network
# learns paths -- so this is not a heartbeat. It is slow enough to be cheap and
# frequent enough that a node restarting is found again without anyone doing
# anything. Members are kept far longer than this (see tak_membership), so a
# missed announce costs nothing.
ANNOUNCE_INTERVAL_SECONDS = 30 * 60
# Loopback only, and not configurable. This endpoint applies no authentication
# because it assumes only this device can reach it; binding it to a routable
# address would hand the mesh to anyone who can open a socket.
BIND_HOST = "127.0.0.1"


def load_node_identity(path=None):
    """This node's long-lived identity, created on first use.

    Deliberately not the team identity. Everyone on a team derives that one
    from the shared secret, so using it here would give every member the same
    UID -- one track on the map for the whole team.
    """
    import RNS
    identity_path = Path(path or DEFAULT_IDENTITY_PATH).expanduser()
    identity_path.parent.mkdir(parents=True, exist_ok=True)
    # Opened once and read through that descriptor. Identity.from_file() reopens
    # by name, so validating with lstat() and then handing the *pathname* to RNS
    # checks one file and loads another -- a local process can swap the path in
    # between and choose this node's identity for it. Same pattern as
    # tools/tak_tasking.py.
    try:
        fd = os.open(identity_path, os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError:
        fd = None
    except OSError as error:
        raise ValueError("Node identity must be a regular file: %s (%s)"
                         % (identity_path, error.strerror)) from error
    if fd is not None:
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode):
                raise ValueError("Node identity must be a regular file: %s" % identity_path)
            if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
                raise ValueError("Node identity is group- or world-accessible: %s"
                                 % identity_path)
            with os.fdopen(os.dup(fd), "rb") as handle:
                key = handle.read()
        finally:
            os.close(fd)
        identity = RNS.Identity.from_bytes(key)
        if identity is None:
            raise ValueError("Node identity file is not a valid identity: %s" % identity_path)
        return identity
    identity = RNS.Identity()
    # Written through a private-by-construction descriptor rather than written
    # and then chmod'ed: the gap between the two is a window where the key is
    # readable by anyone on the box.
    handle = os.open(identity_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(handle)
    identity.to_file(str(identity_path))
    os.chmod(identity_path, 0o600)
    return identity


class TeamAnnounceHandler:
    """Learns team members from the announces every node already hears.

    Reticulum hands every announce on our aspect to this, most of which are not
    ours: another team, or software that is not this project at all. The
    registry decides, and a payload it does not recognise is ordinary.
    """

    def __init__(self, registry, on_new_member=None):
        self.aspect_filter = "%s.%s" % (tak_identity.NODE_APP,
                                        ".".join(tak_identity.NODE_ASPECTS))
        self.registry = registry
        self.on_new_member = on_new_member

    def received_announce(self, destination_hash, announced_identity, app_data):
        if self.registry.remember(destination_hash, app_data) is membership.MEMBER_NEW:
            if self.on_new_member:
                self.on_new_member(destination_hash)


class CotBridge:
    def __init__(self, team, secret, port=DEFAULT_PORT, identity_path=None,
                 callsign="BRIDGE", role="Team Member"):
        import RNS
        self.rns = RNS
        self.team = team
        self.secret = secret
        self.callsign = callsign
        self.role = role
        self.port = port
        self.clients = []
        self.clients_lock = threading.Lock()
        self.sent = self.received = 0
        # Two separate counts. They mean different things: one is a
        # frame off the mesh this build cannot read, which is ordinary
        # on a shared destination; the other is an event ATAK sent that
        # we refused. Collapsing them hides whichever is smaller.
        self.unreadable = 0
        self.unreachable = 0

        # The UID names *this node*, not the team. Registered IN so the address
        # a peer recovers from the UID is one this node actually answers on --
        # and so team traffic addressed to us arrives here.
        self.identity = load_node_identity(identity_path)
        self.node = tak_identity.node_destination(self.identity, RNS.Destination.IN)
        self.node.set_packet_callback(self._from_mesh)
        self.uid = tak_identity.uid_for(self.node.hash)

        # No GROUP destination. Pivot 5: a group packet reaches only peers on
        # the same interface as the sender -- any intermediary at all spends
        # its single hop -- so a team is a membership set and traffic for it is
        # addressed to each member. See docs/TAKIntegrationPivots.md.
        self.registry = membership.MemberRegistry(team, secret, own_hash=self.node.hash)
        self.announce_handler = TeamAnnounceHandler(self.registry, self._member_joined)
        RNS.Transport.register_announce_handler(self.announce_handler)

        # One pipeline per bridge: it holds the learned ATAK UID, refuses our
        # own events before they can teach it anything, and rewrites our
        # self-reports. The order of those three is the whole of it.
        self.pipeline = CotOutbound(self.uid)
        self._announce_thread = None

    # ---- membership ----
    def announce(self):
        """Say who we are, so peers can address us.

        Without this the UID derived in pivot 1 decodes to a destination
        nothing has a path to: correct, and unreachable. The payload carries an
        HMAC of the team rather than its name, so the announce does not undo
        what group_aspects went to trouble to hide.
        """
        self.node.announce(membership.member_payload(self.team, self.secret,
                                                     self.callsign, self.role))

    def _member_joined(self, destination_hash):
        claims = self.registry.describe(destination_hash) or {}
        print("[bridge] team member %s is %s"
              % (claims.get("callsign", "?"), tak_identity.uid_for(destination_hash)),
              flush=True)
        # Say who we are back. A node that starts late hears everyone who
        # announces after it and nobody who announced before, so without this
        # the first node up stays invisible to the second until the next
        # re-announce -- half an hour of a team that cannot see its own
        # members. The registry rate-limits, and greeting only happens for a
        # member that was not already known, so the reply this provokes finds a
        # known member and goes no further.
        if self.registry.should_greet():
            try:
                self.announce()
            except Exception as error:                      # noqa: BLE001
                print("[bridge] greeting announce failed: %s" % error, flush=True)

    def _announce_forever(self):
        while True:
            time.sleep(ANNOUNCE_INTERVAL_SECONDS)
            try:
                self.announce()
            except Exception as error:                      # noqa: BLE001
                # A failed announce is not worth losing the bridge over; the
                # next one is half an hour away and members are kept far longer.
                print("[bridge] announce failed: %s" % error, flush=True)

    # ---- mesh -> ATAK ----
    def _from_mesh(self, data, packet):
        try:
            xml = tier2.decode(bytes(data))
        except ValueError:
            # A frame we cannot read is ordinary on a shared destination: an
            # older node, a newer dictionary, or simply not ours.
            self.unreadable += 1
            return
        self._to_clients(xml.encode("utf-8"))
        self.received += 1

    def _to_clients(self, payload):
        """Write to every connected client without blocking the ones behind.

        This runs on Reticulum's packet callback thread. sendall() blocks for as
        long as a client refuses to read -- an ATAK that has stopped draining
        its socket blocks forever -- and doing that while holding clients_lock
        would stop every other client, the accept loop, and the mesh callback
        that delivers the next packet. So the lock covers the snapshot only, and
        a client that cannot keep up is dropped rather than allowed to stall the
        bridge.
        """
        with self.clients_lock:
            targets = list(self.clients)
        dead = []
        for client in targets:
            try:
                # A timeout rather than a blocking write, because "slow" and
                # "gone" look identical from here and only one of them resolves
                # itself. A client that cannot take a single CoT event inside
                # this window is not keeping up with a live feed either.
                client.settimeout(CLIENT_WRITE_TIMEOUT)
                client.sendall(payload)
            except OSError:
                dead.append(client)
        if not dead:
            return
        with self.clients_lock:
            for client in dead:
                if client in self.clients:
                    self.clients.remove(client)
        for client in dead:
            try:
                client.close()
            except OSError:
                pass

    # ---- ATAK -> mesh ----
    def _from_atak(self, xml):
        known = self.pipeline.atak_uid
        frame = self.pipeline.frame(xml, tier2.encode)
        if known is None and self.pipeline.atak_uid:
            print("[bridge] this ATAK calls itself %s; peers will see %s"
                  % (self.pipeline.atak_uid, self.uid), flush=True)
        if frame is None:
            # The pipeline keeps its own refusal count; reading it here rather
            # than assigning it into a shared one, which silently discarded
            # every mesh-side drop recorded above.
            return
        self.sent += self._fan_out(frame)

    def _fan_out(self, frame):
        """Send one frame to every member of the team. Returns how many went.

        One routed unicast each, because that is the only thing that crosses a
        hop. The airtime is real and is the reason position does not come this
        way: a marker is an operator action and rare, a position report is a
        beacon. See pivot 5 for the costing.
        """
        sent = 0
        for destination_hash in self.registry.members():
            identity = self.rns.Identity.recall(destination_hash)
            if identity is None:
                # Heard the announce, lost the identity -- possible after a
                # restart. Ask for the path; the next marker will find it.
                self.rns.Transport.request_path(destination_hash)
                self.unreachable += 1
                continue
            try:
                destination = tak_identity.node_destination(identity,
                                                            self.rns.Destination.OUT)
                self.rns.Packet(destination, frame).send()
                sent += 1
            except OSError as error:
                # An oversized frame is refused by encode() long before here,
                # so this is a transport problem: no path, or an interface that
                # has gone. Counted rather than fatal -- one unreachable member
                # must not cost the others their copy.
                self.unreachable += 1
                print("[bridge] could not reach %s: %s"
                      % (destination_hash.hex(), error), flush=True)
        return sent

    def _serve_client(self, connection):
        stream = CotStream()
        with self.clients_lock:
            self.clients.append(connection)
        try:
            while True:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                for event in stream.feed(chunk):
                    self._from_atak(event)
        except OSError:
            pass
        finally:
            with self.clients_lock:
                if connection in self.clients:
                    self.clients.remove(connection)
            try:
                connection.close()
            except OSError:
                pass

    def serve_forever(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((BIND_HOST, self.port))
        listener.listen(8)
        print("[bridge] team %r, this node is %s (%s)"
              % (self.team, self.uid, self.callsign), flush=True)
        self.announce()
        self._announce_thread = threading.Thread(target=self._announce_forever, daemon=True)
        self._announce_thread.start()
        print("[bridge] point ATAK at %s:%d, TCP, no SSL" % (BIND_HOST, self.port), flush=True)
        while True:
            connection, _ = listener.accept()
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self._serve_client, args=(connection,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--team", default=tak_identity.DEFAULT_TEAM)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--secret-file", default=None,
                        help="path to the fleet secret; %s is preferred"
                             % groups.SECRET_ENVIRONMENT)
    parser.add_argument("--config", default=None, help="Reticulum config directory")
    parser.add_argument("--callsign", default="BRIDGE",
                        help="how this node identifies itself to the team")
    parser.add_argument("--role", default="Team Member")
    parser.add_argument("--identity", default=None,
                        help="this node's identity file (default %s)" % DEFAULT_IDENTITY_PATH)
    args = parser.parse_args()

    secret = groups.load_fleet_secret(args.secret_file)
    import RNS
    reticulum = RNS.Reticulum(args.config)
    if reticulum.is_connected_to_shared_instance:
        # Refused rather than warned about. A GROUP packet is dropped above one
        # hop, and a shared-instance client sits one hop behind the daemon that
        # owns the interfaces -- so a peer one hop from the daemon is two hops
        # from here and every packet is dropped on delivery. Nothing reports
        # it: the endpoint listens, the team address is right, counters move on
        # the wire, and no CoT ever arrives. Measured 2026-09-11; it cost an
        # afternoon, and it would cost it again every time.
        print("[bridge] REFUSING to run as a client of a shared Reticulum instance.",
              flush=True)
        print("[bridge] Group traffic cannot survive the extra hop that adds. Give",
              flush=True)
        print("[bridge] the bridge its own config with share_instance = No and its",
              flush=True)
        print("[bridge] own interface, and point peers at that:  --config <dir>",
              flush=True)
        sys.exit(1)
    bridge = CotBridge(args.team, secret, args.port, args.identity,
                       args.callsign, args.role)
    try:
        bridge.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] sent %d, received %d, unreadable %d, refused %d, "
              "unreachable %d, members %d"
              % (bridge.sent, bridge.received, bridge.unreadable,
                 bridge.pipeline.dropped, bridge.unreachable,
                 len(bridge.registry)), flush=True)


if __name__ == "__main__":
    main()
