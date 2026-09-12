"""Local heartbeat replies must not depend on mesh activity or reach peers."""
import socket
import select
import sys
import threading
import unittest
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_endpoint import CotClient, CotStream, ping_reply

PING = b'<event uid="ANDROID-test" type="t-x-c-t"><detail/></event>'
MARKER = b'<event uid="marker" type="a-h-G"><detail/></event>'


class HeartbeatTests(unittest.TestCase):
    def test_reply_is_a_fresh_valid_pong_preserving_escaped_uid(self):
        now = datetime(2026, 9, 12, tzinfo=timezone.utc)
        reply = ET.fromstring(ping_reply(PING.replace(b'ANDROID-test', b'a&amp;&quot;b'), now))
        self.assertEqual(reply.get("type"), "t-x-c-t-r")
        self.assertEqual(reply.get("uid"), 'a&"b')
        self.assertEqual(reply.get("time"), "2026-09-12T00:00:00.000Z")
        self.assertEqual(reply.get("start"), reply.get("time"))
        self.assertEqual(reply.get("stale"), "2026-09-12T00:01:00.000Z")
        self.assertIsNotNone(reply.find("point"))
        self.assertIsNotNone(reply.find("detail"))

    def test_only_root_ping_type_is_answered(self):
        for xml in (MARKER, PING.replace(b't-x-c-t', b't-x-c-t-r'),
                    b'<event uid="t-x-c-t"><detail type="t-x-c-t"/></event>',
                    b'<event type="t-x-c-t"><broken></event>', b'\xff',
                    b'<!DOCTYPE event><event type="t-x-c-t"/>'):
            self.assertIsNone(ping_reply(xml), xml)

    def test_self_closed_ping_never_swallows_the_next_event(self):
        ping = b'<event uid="a>b" type="t-x-c-t"/>'
        wire = ping + PING + MARKER
        for cut in range(len(wire) + 1):
            stream = CotStream()
            self.assertEqual(stream.feed(wire[:cut]) + stream.feed(wire[cut:]),
                             [ping, PING, MARKER], cut)

    def test_concurrent_writers_cannot_interleave_events(self):
        entered = threading.Event()
        release = threading.Event()
        second_entered = threading.Event()
        class SlowSocket:
            def __init__(self):
                self.data = []
            def sendall(self, payload):
                if payload == b"xyz":
                    second_entered.set()
                self.data.append(payload[:1])
                if payload == b'abc':
                    entered.set()
                    release.wait(2)
                self.data.append(payload[1:])
        connection = SlowSocket()
        client = CotClient(connection)
        first = threading.Thread(target=client.sendall, args=(b'abc',))
        second = threading.Thread(target=client.sendall, args=(b'xyz',))
        first.start()
        self.assertTrue(entered.wait(2))
        second.start()
        try:
            self.assertFalse(second_entered.wait(0.05), "second writer entered mid-event")
        finally:
            release.set()
            first.join(2)
            second.join(2)
        self.assertEqual(b''.join(connection.data), b'abcxyz')


class BridgeHeartbeatTests(unittest.TestCase):
    def test_repeated_fragmented_pings_stay_local_and_connection_still_delivers(self):
        from cot_bridge import CotBridge
        bridge = CotBridge.__new__(CotBridge)
        bridge.clients = []
        bridge.clients_lock = threading.Lock()
        bridge._from_atak = Mock()
        threads = []
        phones = []
        for _ in range(2):
            endpoint, phone = socket.socketpair()
            phone.settimeout(2)
            phones.append(phone)
            thread = threading.Thread(target=bridge._serve_client, args=(endpoint,), daemon=True)
            threads.append(thread)
            thread.start()
        def cleanup():
            for phone in phones:
                phone.close()
            for thread in threads:
                thread.join(2)
        self.addCleanup(cleanup)
        def receive(phone):
            stream = CotStream()
            while True:
                chunk = phone.recv(4096)
                self.assertTrue(chunk, "endpoint disconnected")
                events = stream.feed(chunk)
                if events:
                    self.assertEqual(len(events), 1)
                    return events[0]
        # A reply on each socket also proves both are registered before fan-out.
        for phone in phones:
            for _ in range(3):
                phone.sendall(PING[:17])
                phone.sendall(PING[17:])
                self.assertEqual(ET.fromstring(receive(phone)).get('type'), 't-x-c-t-r')
        bridge._from_atak.assert_not_called()
        # Idle longer than the send timeout: it must never become a read timeout.
        self.assertEqual(select.select(phones, [], [], 2.1)[0], [])
        phones[0].sendall(MARKER + PING)
        receive(phones[0])  # The marker is processed before this reply.
        bridge._from_atak.assert_called_once_with(MARKER)
        bridge._to_clients(MARKER)
        for phone in phones:
            self.assertEqual(receive(phone), MARKER)

    def test_no_clients_is_reported_as_delivery_loss(self):
        from cot_bridge import CotBridge
        bridge = CotBridge.__new__(CotBridge)
        bridge.clients = []
        bridge.clients_lock = threading.Lock()
        with patch('builtins.print') as log:
            bridge._to_clients(MARKER)
        self.assertIn('delivery lost', log.call_args.args[0])


if __name__ == '__main__':
    unittest.main()
