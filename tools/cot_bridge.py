#!/usr/bin/env python3
"""Run a local CoT endpoint: ATAK on 127.0.0.1, a team on the mesh.

    TAK_FLEET_SECRET=... tools/cot_bridge.py --team Cyan

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
from cot_endpoint import CotOutbound, CotStream

DEFAULT_PORT = 8087
# This node's own Reticulum identity, kept beside the fleet secret. Created on
# first run; losing it changes this node's UID, which to every peer reads as a
# different responder rather than the same one returning.
DEFAULT_IDENTITY_PATH = "~/.impr-tak/node-identity"
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
    if identity_path.exists():
        info = identity_path.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
            raise ValueError("Node identity must be a regular file: %s" % identity_path)
        if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise ValueError("Node identity is group- or world-accessible: %s" % identity_path)
        return RNS.Identity.from_file(str(identity_path))
    identity = RNS.Identity()
    # Written through a private-by-construction descriptor rather than written
    # and then chmod'ed: the gap between the two is a window where the key is
    # readable by anyone on the box.
    handle = os.open(identity_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(handle)
    identity.to_file(str(identity_path))
    os.chmod(identity_path, 0o600)
    return identity


class CotBridge:
    def __init__(self, team, secret, port=DEFAULT_PORT, identity_path=None):
        import RNS
        self.rns = RNS
        self.team = team
        self.port = port
        self.clients = []
        self.clients_lock = threading.Lock()
        self.sent = self.received = self.dropped = 0

        # A GROUP destination is symmetric -- every member both speaks and
        # listens on it, and command is a member rather than a hop -- but RNS
        # still wants an IN and an OUT object. Both are built from the team's
        # shared identity so every member lands on the same address.
        self.group = groups.group_destination(team, secret, RNS.Destination.IN)
        self.group.set_packet_callback(self._from_mesh)
        self.out = groups.group_destination(team, secret, RNS.Destination.OUT)

        # The UID names *this node*, not the team. Registered IN so the address
        # a peer recovers from the UID is one this node actually answers on.
        self.identity = load_node_identity(identity_path)
        self.node = tak_identity.node_destination(self.identity, RNS.Destination.IN)
        self.uid = tak_identity.uid_for(self.node.hash)
        # One pipeline per bridge: it holds the learned ATAK UID, refuses our
        # own events before they can teach it anything, and rewrites our
        # self-reports. The order of those three is the whole of it.
        self.pipeline = CotOutbound(self.uid)

    # ---- mesh -> ATAK ----
    def _from_mesh(self, data, packet):
        try:
            xml = tier2.decode(bytes(data))
        except ValueError:
            # A frame we cannot read is ordinary on a shared destination: an
            # older node, a newer dictionary, or simply not ours.
            self.dropped += 1
            return
        self._to_clients(xml.encode("utf-8"))
        self.received += 1

    def _to_clients(self, payload):
        with self.clients_lock:
            for client in list(self.clients):
                try:
                    client.sendall(payload)
                except OSError:
                    self.clients.remove(client)
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
            self.dropped = self.pipeline.dropped
            return
        self.rns.Packet(self.out, frame).send()
        self.sent += 1

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
        print("[bridge] team %r on %s" % (self.team, self.group.hash.hex()), flush=True)
        print("[bridge] this node is %s" % self.uid, flush=True)
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
    parser.add_argument("--identity", default=None,
                        help="this node's identity file (default %s)" % DEFAULT_IDENTITY_PATH)
    args = parser.parse_args()

    secret = groups.load_fleet_secret(args.secret_file)
    import RNS
    RNS.Reticulum(args.config)
    bridge = CotBridge(args.team, secret, args.port, args.identity)
    try:
        bridge.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] sent %d, received %d, dropped %d"
              % (bridge.sent, bridge.received, bridge.dropped), flush=True)


if __name__ == "__main__":
    main()
