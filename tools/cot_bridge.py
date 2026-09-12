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
import struct
from datetime import datetime, timedelta, timezone
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
import tak_lxmf
import cot_chat
import cot_gateway
import cot_marker
import cot_position
import position_codec
import tak_membership as membership
import tak_payload
from cot_endpoint import CotOutbound, CotStream

DEFAULT_PORT = 8087
# This node's own Reticulum identity, kept beside the fleet secret. Created on
# first run; losing it changes this node's UID, which to every peer reads as a
# different responder rather than the same one returning.
DEFAULT_IDENTITY_PATH = "~/.impr-tak/node-identity"
# How long a single write to one ATAK client may take before that client is
# treated as gone. Generous for a loopback socket carrying a few hundred bytes,
# and short enough that one stalled client cannot hold up the mesh callback
# thread that every other client's data arrives on. Applied with SO_SNDTIMEO so
# it bounds sends only; see _serve_client for why that distinction matters.
CLIENT_WRITE_TIMEOUT = 2.0
# How often this node re-announces its membership.
#
# Announces are deliberately expensive in Reticulum -- they are how the network
# learns paths -- so this is not a heartbeat. It is slow enough to be cheap and
# frequent enough that a node restarting is found again without anyone doing
# anything. Members are kept far longer than this (see tak_membership), so a
# missed announce costs nothing.
ANNOUNCE_INTERVAL_SECONDS = 30 * 60
# How long a position is worth drawing before the track should go grey.
#
# Twice the report floor: one missed report is a radio being a radio, two is
# worth an operator noticing. A stale track that never expires is the failure
# that matters here -- a marker where somebody used to be, still being trusted.
POSITION_STALE_SECONDS = 2 * cot_position.DEFAULT_INTERVAL_SECONDS
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
                 callsign="BRIDGE", role="Team Member",
                 lxmf_storage=None, propagation_node=None):
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

        # Position does not go through tier 2. 94% of what ATAK emits is a
        # position report, and at ten nodes over four hops that is 85% of a
        # LoRa channel as compressed CoT against 32% as this codec -- neither
        # survivable alone, which is why the gate matters as much as the
        # encoding. See tools/cot_position.py for the measurements.
        self.position_gate = cot_position.PositionGate()
        self.sender_id = self.registry.sender_id_for(self.node.hash)
        self.positions_sent = self.positions_received = 0
        self.chat_sent = self.chat_received = 0
        self.chat_undeliverable = 0
        self.markers_sent = self.markers_received = 0
        # SPI is tier 1 by the classification in TAKNative.md -- a pointer
        # ATAK drags across the map, 121 of the 853 captured events, and
        # latest-wins. Gated like position, and for the same reason: the
        # codec makes each one cheap, the gate is what makes the stream of
        # them affordable.
        self.spi_gate = cot_position.PositionGate(interval_seconds=5)

        # A direct message goes by LXMF, which brings the three things a bare
        # Packet does not: proof-backed delivery, retry, and a propagation node
        # that holds a line for somebody out of range. A room line does not --
        # see tools/tak_lxmf.py for why that split is a property of broadcast
        # rather than a shortcut.
        self.lxmf = None
        if lxmf_storage:
            self.lxmf = tak_lxmf.Carrier(self.identity, lxmf_storage, self.callsign,
                                         self._chat_from_lxmf,
                                         propagation_node=propagation_node)
        # Receipts for a room never leave this node. Every member answering a
        # room line with a delivery and a read receipt is two thirds of group
        # chat's airtime -- 7.2 s of the 11.1 s a ten-person room costs -- for
        # one bit of meaning each. A direct message keeps both, because there
        # it is one peer and the operator is waiting on exactly that answer.
        self.receipts_suppressed = 0

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
        raw = bytes(data)
        # Byte zero says which codec produced this. One namespace shared by all
        # of them rather than three independent version counters -- see
        # tools/tak_payload.py for why that distinction matters.
        kind = tak_payload.kind_of(raw)
        if kind == tak_payload.POSITION_V2:
            if self._position_from_mesh(raw):
                return
        elif kind == tak_payload.CHAT_V1:
            if self._chat_from_mesh(raw):
                return
        elif kind == tak_payload.MARKER_V1:
            if self._marker_from_mesh(raw):
                return
        try:
            xml = tier2.decode(raw)
        except ValueError:
            # A frame we cannot read is ordinary: an older node, a newer
            # dictionary, or simply not ours.
            self.unreadable += 1
            return
        self._to_clients(xml.encode("utf-8"))
        self.received += 1

    def _marker_from_mesh(self, raw):
        """Render a peer's marker as CoT for the local ATAK."""
        decoded = cot_marker.decode(raw)
        if decoded is None:
            return False
        sender = self.registry.resolve_sender_id(decoded["sender_id"])
        if sender is None:
            # A marker from a node this team has never heard announce. Drawing
            # it under an invented identity would put an object on the map that
            # nobody can be asked about.
            self.unreadable += 1
            return True
        claims = self.registry.describe(sender) or {}
        now = datetime.now(timezone.utc)
        self._to_clients(cot_marker.build_marker_cot(
            decoded, tak_identity.uid_for(sender), claims.get("callsign", "UNKNOWN"),
            cot_gateway.cot_time(now),
            cot_gateway.cot_time(now + timedelta(seconds=decoded["stale_seconds"]))
        ).encode("utf-8"))
        self.markers_received += 1
        return True

    def _chat_from_mesh(self, raw):
        """Render a peer's chat line or receipt as CoT for the local ATAK."""
        decoded = cot_chat.decode(raw)
        if decoded is None:
            return False
        sender = self.registry.resolve_sender_id(decoded["sender_id"])
        if sender is None:
            # Chat from a node this team has never heard announce. Dropping it
            # is the honest option: rendering it under an invented identity
            # would put words on the screen attributed to nobody.
            self.unreadable += 1
            return True
        claims = self.registry.describe(sender) or {}
        self._to_clients(cot_chat.build_chat_cot(
            decoded, tak_identity.uid_for(sender),
            claims.get("callsign", "UNKNOWN"),
            cot_gateway.cot_time(datetime.now(timezone.utc))).encode("utf-8"))
        self.chat_received += 1
        return True

    def _position_from_mesh(self, raw):
        """Render a peer's position report as CoT for the local ATAK."""
        fix = position_codec.decode(raw)
        if fix is None:
            return False
        # Four bytes of identity is a lookup key here, not an identity.
        # Membership is the table that turns it back into a whole destination
        # hash, so a peer's track carries the same UID as everything else that
        # node sends rather than a track of its own.
        sender = self.registry.resolve_sender_id(fix.sender_id)
        if sender is None:
            self.unreadable += 1
            return True
        claims = self.registry.describe(sender) or {}
        self._to_clients(cot_gateway.build_cot(
            fix, tak_identity.uid_for(sender), claims.get("callsign", "UNKNOWN"),
            POSITION_STALE_SECONDS, team=self.team))
        self.positions_received += 1
        return True

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
        # The echo guard first, for everything, before anything is learned from
        # the event or spent on it. Our own self-report coming back is a
        # well-formed self-report, and learning from it would teach this
        # pipeline our own UID.
        if self.pipeline.is_echo(xml):
            return
        # Then learn, and only then route. This used to be a side effect of the
        # tier-2 path, which the typed codecs return before ever reaching --
        # so a session that opened with an SPI put ANDROID-<device>.SPI1 on the
        # air as tier 2, bypassing the very scrubbing the marker codec added.
        # The typed handlers do not need the learned UID; only the tier-2
        # rewrite does, and now both get it.
        known = self.pipeline.atak_uid
        self.pipeline.observe(xml)
        if known is None and self.pipeline.atak_uid:
            print("[bridge] this ATAK calls itself %s; peers will see %s"
                  % (self.pipeline.atak_uid, self.uid), flush=True)
        if cot_position.is_position(xml) and self._position_from_atak(xml):
            return
        if self._chat_from_atak(xml):
            return
        if self._marker_from_atak(xml):
            return
        frame = self.pipeline.frame(xml, tier2.encode)
        if frame is None:
            # The pipeline keeps its own refusal count; reading it here rather
            # than assigning it into a shared one, which silently discarded
            # every mesh-side drop recorded above.
            return
        self.sent += self._fan_out(frame)

    def _position_from_atak(self, xml):
        """Send a position report as twenty-one bytes, if it is due.

        True when this event was handled here, whether or not anything went on
        the air: a suppressed report is handled, and must not then also be sent
        as CoT.
        """
        fix = cot_position.fix_from_cot(xml, self.sender_id)
        if fix is None:
            # Shaped like a position and carrying none. Not ours to encode, so
            # it falls through to tier 2 rather than being dropped.
            return False
        if not self.position_gate.allows(fix, time.time()):
            return True
        self.positions_sent += self._fan_out(position_codec.encode(fix))
        return True

    def _chat_from_atak(self, xml):
        """Send a chat line or a receipt as tens of bytes, if it is one.

        True when this event was handled here. Chat is worth its own codec for
        a reason tier 2 makes plain: a real GeoChat line compresses to 402
        bytes against a 383-byte MDU, so before this it was not expensive, it
        was undeliverable.
        """
        frame = cot_chat.chat_from_cot(xml, self.sender_id)
        if frame is None:
            return False
        decoded = cot_chat.decode(frame)
        # A direct message goes to one member, not to the team. ATAK puts the
        # recipient's callsign in the chatroom field, so without the recipient
        # carried separately every private line was fanned out to everybody --
        # not a cost problem, a confidentiality one.
        #
        # This works because peers are announced under their Reticulum-rooted
        # UID, so ATAK addresses them by it and destination_for() reverses it.
        # That is pivot 1 paying for itself.
        recipient = (decoded or {}).get("recipient") or ""
        if not recipient and (decoded or {}).get("kind") != cot_chat.KIND_MESSAGE:
            # A receipt for a room line. Dropped here rather than sent: every
            # member answering a broadcast with two receipts is two thirds of
            # what group chat costs on the air, and none of it tells an
            # operator anything they can act on. A direct receipt still goes.
            self.receipts_suppressed += 1
            return True
        if recipient:
            destination = tak_identity.destination_for(recipient)
            if destination is None:
                # Addressed to somebody who is not a peer of ours: a server
                # contact, or a callsign this mesh has never announced. The
                # fan-out is not a fallback here -- broadcasting a line meant
                # for one person is the bug this whole path exists to stop,
                # and it would not deliver it either. Counted and dropped, and
                # anything reachable another way is still reached that way.
                self.chat_undeliverable += 1
                print("[bridge] chat for %r is not a member of this team, not sent"
                      % recipient, flush=True)
                return True
            # LXMF first, because it is the only path that can promise the
            # line arrives. The bare packet is the fallback for a peer whose
            # identity we cannot recall, not the normal route.
            identity = self.rns.Identity.recall(destination)
            if self.lxmf is not None and identity is not None:
                text = (decoded or {}).get("text") or ""
                if self.lxmf.send_chat(identity, frame, text):
                    self.chat_sent += 1
                    return True
            self.chat_sent += self._send_to(destination, frame)
            return True
        self.chat_sent += self._fan_out(frame)
        return True

    def _chat_from_lxmf(self, frame, source_hash):
        """A chat line that arrived over LXMF rather than as a bare packet.

        Same rendering as the packet path: the frame is the protocol, whichever
        carrier brought it. The LXMF content is for the human reading Columba
        and is deliberately never parsed back here -- that is what stops a
        rendered message being read as a new one.
        """
        if self._chat_from_mesh(frame):
            self.received += 1

    def _marker_from_atak(self, xml):
        """Send a point marker as tens of bytes, if it is one.

        SPI passes through a cadence gate first. It is a pointer being dragged
        across a map, so most of what ATAK emits is a position it has already
        superseded, and sending every one would spend the channel on a cursor.
        """
        frame = cot_marker.marker_from_cot(xml, self.sender_id)
        if frame is None:
            return False
        decoded = cot_marker.decode(frame)
        if decoded and decoded["type"] == cot_marker.SPI_TYPE:
            fix = position_codec.PositionFix(lat_e7=decoded["lat_e7"],
                                             lon_e7=decoded["lon_e7"])
            if not self.spi_gate.allows(fix, time.time()):
                # Handled: suppressed rather than passed on to tier 2, which
                # would spend more airtime than the codec just saved.
                return True
        self.markers_sent += self._fan_out(frame)
        return True

    def _send_to(self, destination_hash, frame):
        """Send one frame to one node. Returns 1 if it went, 0 if it did not.

        Shared with the fan-out so an addressed line and a broadcast line take
        the same path -- a second copy of this would be a second place for the
        recall-and-request-path dance to be got wrong.
        """
        identity = self.rns.Identity.recall(destination_hash)
        if identity is None:
            # Heard the announce, lost the identity -- possible after a
            # restart. Ask for the path; the next event will find it.
            self.rns.Transport.request_path(destination_hash)
            self.unreachable += 1
            return 0
        try:
            destination = tak_identity.node_destination(identity,
                                                        self.rns.Destination.OUT)
            self.rns.Packet(destination, frame).send()
            return 1
        except OSError as error:
            # An oversized frame is refused by encode() long before here, so
            # this is a transport problem: no path, or an interface that has
            # gone. Counted rather than fatal -- one unreachable member must
            # not cost the others their copy.
            self.unreachable += 1
            print("[bridge] could not reach %s: %s"
                  % (destination_hash.hex(), error), flush=True)
            return 0

    def _fan_out(self, frame):
        """Send one frame to every member of the team. Returns how many went.

        One routed unicast each, because that is the only thing that crosses a
        hop. The airtime is real and is the reason position does not come this
        way: a marker is an operator action and rare, a position report is a
        beacon. See pivot 5 for the costing.
        """
        return sum(self._send_to(member, frame) for member in self.registry.members())

    def _serve_client(self, connection):
        stream = CotStream()
        # SO_SNDTIMEO, not settimeout(). A timeout set with settimeout() belongs
        # to the whole socket, so the write bound also bounds recv() -- and this
        # loop reads on the same object, catching the resulting timeout as a
        # dead client. Every client was disconnected two seconds after receiving
        # its first message, which on the bench looked like chat not working.
        # SO_SNDTIMEO bounds sends in the kernel and leaves reads blocking.
        connection.setsockopt(
            socket.SOL_SOCKET, socket.SO_SNDTIMEO,
            struct.pack("@qq", int(CLIENT_WRITE_TIMEOUT), 0))
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
    parser.add_argument("--lxmf-storage", default=None,
                        help="LXMF router storage directory; direct messages "
                             "go by LXMF when this is set, which is what gives "
                             "them receipts, retry and store-and-forward. "
                             "Defaults to <config>/lxmf.")
    parser.add_argument("--no-lxmf", action="store_true",
                        help="send direct messages as bare packets, as PR B "
                             "did. Best effort, and nothing will say when a "
                             "line is lost.")
    parser.add_argument("--propagation-node", default=None,
                        help="destination hash of an LXMF propagation node, "
                             "which is what holds a line for a peer who is out "
                             "of range until they come back")
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
    # LXMF keeps its own store -- the queue for a peer who is out of range is
    # the whole point, and a queue that does not outlive a restart is not one.
    lxmf_storage = None
    if not args.no_lxmf:
        lxmf_storage = args.lxmf_storage or os.path.join(
            os.path.expanduser(args.config or "~/.reticulum"), "lxmf")
        os.makedirs(lxmf_storage, exist_ok=True)
    propagation_node = None
    if args.propagation_node:
        try:
            propagation_node = bytes.fromhex(args.propagation_node)
        except ValueError:
            sys.exit("--propagation-node is not a hex destination hash: %r"
                     % args.propagation_node)
    bridge = CotBridge(args.team, secret, args.port, args.identity,
                       args.callsign, args.role,
                       lxmf_storage=lxmf_storage,
                       propagation_node=propagation_node)
    try:
        bridge.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] sent %d, received %d, positions %d/%d, chat %d/%d, "
              "markers %d/%d, unreadable %d, refused %d, unreachable %d, "
              "not a member %d, receipts dropped %d, lxmf %d/%d/%d, "
              "suppressed %d/%d, members %d"
              % (bridge.sent, bridge.received,
                 bridge.positions_sent, bridge.positions_received,
                 bridge.chat_sent, bridge.chat_received,
                 bridge.markers_sent, bridge.markers_received,
                 bridge.unreadable, bridge.pipeline.dropped, bridge.unreachable,
                 bridge.chat_undeliverable, bridge.receipts_suppressed,
                 (bridge.lxmf.sent if bridge.lxmf else 0),
                 (bridge.lxmf.delivered if bridge.lxmf else 0),
                 (bridge.lxmf.failed if bridge.lxmf else 0),
                 bridge.position_gate.suppressed, bridge.spi_gate.suppressed,
                 len(bridge.registry)), flush=True)
        # The LXMF store holds anything still queued for a peer who is out of
        # range. Closing the router is what flushes it to disk, so a queue
        # survives the restart it exists to survive.
        if bridge.lxmf is not None:
            bridge.lxmf.stop()


if __name__ == "__main__":
    main()
