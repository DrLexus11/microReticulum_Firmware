"""What the endpoint holds for an ATAK that is not attached yet.

Columba is reliable because LXMF persists a message. ATAK is handed a live
stream and kept nothing, so anything rendered while it was detached was gone --
and ATAK detaches routinely, for seconds on a reconnect and for minutes when a
phone is locked. Seen twice on hardware 2026-09-12: a reply crossed two LoRa
hops, was decoded and rebuilt correctly, and was dropped at the socket because
nothing was listening.
"""

import collections
import socket
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge, REPLAY_MAX_AGE_SECONDS  # noqa: E402


def bridge():
    """A bridge with only the parts the socket path touches."""
    made = CotBridge.__new__(CotBridge)
    made.clients = []
    made.clients_lock = threading.Lock()
    made._replay = collections.deque(maxlen=64)
    made._replay_lock = threading.Lock()
    made.replayed = 0
    return made


class ReplayTests(unittest.TestCase):
    def test_a_tier_two_event_is_held_when_nobody_is_listening(self):
        made = bridge()
        made._to_clients(b"<event/>", keep=True)
        self.assertEqual(len(made._replay), 1)

    def test_a_position_is_not_held(self):
        """Latest-wins, and a fresher one is seconds away. Replaying a
        ten-minute-old fix as though it were current puts somebody on the map
        where they are not, which is worse than showing nothing. The tiering in
        TAKNative.md drew this line; this is the same line at the socket."""
        made = bridge()
        made._to_clients(b"<event type='a-f-G-U-C'/>")
        self.assertEqual(len(made._replay), 0)

    def test_a_client_that_attaches_is_handed_what_it_missed(self):
        made = bridge()
        made._to_clients(b"<event>one</event>", keep=True)
        made._to_clients(b"<event>two</event>", keep=True)
        endpoint, phone = socket.socketpair()
        self.addCleanup(phone.close)
        self.addCleanup(endpoint.close)
        made._replay_to(endpoint)
        phone.settimeout(2)
        self.assertEqual(phone.recv(4096), b"<event>one</event><event>two</event>")
        self.assertEqual(made.replayed, 2)

    def test_oldest_first_so_a_conversation_reads_in_order(self):
        made = bridge()
        for index in range(3):
            made._to_clients(b"<event>%d</event>" % index, keep=True)
        endpoint, phone = socket.socketpair()
        self.addCleanup(phone.close)
        self.addCleanup(endpoint.close)
        made._replay_to(endpoint)
        phone.settimeout(2)
        self.assertEqual(phone.recv(4096), b"<event>0</event><event>1</event><event>2</event>")

    def test_stale_events_are_not_replayed(self):
        """A marker from an hour ago is not news, and a chat line from before
        the operator walked away is context they have already lost."""
        made = bridge()
        made._replay.append((time.time() - REPLAY_MAX_AGE_SECONDS - 1, b"<event>old</event>"))
        made._to_clients(b"<event>new</event>", keep=True)
        endpoint, phone = socket.socketpair()
        self.addCleanup(phone.close)
        self.addCleanup(endpoint.close)
        made._replay_to(endpoint)
        phone.settimeout(2)
        self.assertEqual(phone.recv(4096), b"<event>new</event>")

    def test_the_buffer_is_bounded(self):
        """A bridge nobody is attached to must not grow without limit."""
        made = bridge()
        for index in range(200):
            made._to_clients(b"<event>%d</event>" % index, keep=True)
        self.assertLessEqual(len(made._replay), made._replay.maxlen)

    def test_a_client_that_dies_mid_replay_does_not_take_the_bridge_down(self):
        made = bridge()
        made._to_clients(b"<event>one</event>", keep=True)
        endpoint, phone = socket.socketpair()
        phone.close()
        endpoint.close()
        made._replay_to(endpoint)      # must not raise
        self.assertEqual(made.replayed, 0)

    def test_nothing_held_means_nothing_sent(self):
        made = bridge()
        endpoint, phone = socket.socketpair()
        self.addCleanup(phone.close)
        self.addCleanup(endpoint.close)
        made._replay_to(endpoint)
        phone.settimeout(0.2)
        with self.assertRaises((socket.timeout, TimeoutError)):
            phone.recv(64)


if __name__ == "__main__":
    unittest.main()
