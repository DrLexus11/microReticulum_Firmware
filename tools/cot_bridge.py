#!/usr/bin/env python3
"""Run a local CoT endpoint: ATAK on 127.0.0.1, a team on the mesh.

    TAK_FLEET_SECRET=... tools/cot_bridge.py --team Cyan

ATAK connects to 127.0.0.1:8087 and never to anybody's address. Everything it
sends goes to the team's GROUP destination; everything the team sends is
written back to every connected client. See docs/TAKNative.md.
"""

import argparse
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cot_tier2 as tier2
import tak_groups as groups
import tak_identity as tak_identity
from cot_endpoint import CotStream, learn_atak_uid, rewrite_self_uid

DEFAULT_PORT = 8087
# Loopback only, and not configurable. This endpoint applies no authentication
# because it assumes only this device can reach it; binding it to a routable
# address would hand the mesh to anyone who can open a socket.
BIND_HOST = "127.0.0.1"


class CotBridge:
    def __init__(self, team, secret, port=DEFAULT_PORT):
        import RNS
        self.rns = RNS
        self.team = team
        self.port = port
        self.clients = []
        self.clients_lock = threading.Lock()
        self.atak_uid = None
        self.sent = self.received = self.dropped = 0

        # A GROUP destination is symmetric -- every member both speaks and
        # listens on it, and command is a member rather than a hop -- but RNS
        # still wants an IN and an OUT object. Both are built from the team's
        # shared identity so every member lands on the same address.
        self.group = groups.group_destination(team, secret, RNS.Destination.IN)
        self.group.set_packet_callback(self._from_mesh)
        self.out = groups.group_destination(team, secret, RNS.Destination.OUT)
        self.uid = tak_identity.uid_for(self.group.hash)

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
        if self.atak_uid is None:
            learned = learn_atak_uid(xml)
            if learned:
                self.atak_uid = learned
                print("[bridge] this ATAK calls itself %s; peers will see %s"
                      % (learned, self.uid), flush=True)
        try:
            rewritten = rewrite_self_uid(xml, self.atak_uid, self.uid)
            frame = tier2.encode(rewritten)
        except ValueError as error:
            self.dropped += 1
            print("[bridge] refused an event from ATAK: %s" % error, flush=True)
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
    args = parser.parse_args()

    secret = groups.load_fleet_secret(args.secret_file)
    import RNS
    RNS.Reticulum(args.config)
    bridge = CotBridge(args.team, secret, args.port)
    try:
        bridge.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] sent %d, received %d, dropped %d"
              % (bridge.sent, bridge.received, bridge.dropped), flush=True)


if __name__ == "__main__":
    main()
