"""Files over the mesh: frames, ATAK's notice, the store, the upload API.

The notice below has the shape ATAK 5.8 emitted on the bench 2026-09-22, with
the identifiers replaced.
"""

import hashlib
import io
import os
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error
import urllib.request
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_file_service                 # noqa: E402
import tak_files                        # noqa: E402
import tak_payload                      # noqa: E402
from cot_bridge import CotBridge        # noqa: E402

DATA = b"PK\x03\x04 a data package, or near enough" * 100
HASH = hashlib.sha256(DATA).hexdigest()
ALPHA = b"\x11" * 16
BRAVO = b"\x22" * 16


def notice(file_hash=HASH, dest="ALPHA"):
    return ('<event version="2.0" uid="d8e20920-dff0-4c07-b4f7-0f0f98b9891d" type="b-f-t-r" '
            'access="Undefined" time="2026-09-22T10:39:32.087Z" start="2026-09-22T10:39:32.087Z" '
            'stale="2026-09-22T10:39:42.087Z" how="h-e">'
            '<point lat="41.0" lon="29.0" hae="50" ce="nan" le="nan"/><detail>'
            '<fileshare filename="Recon1.zip" senderUrl="https://192.168.240.1:8443/Marti/api/sync/'
            'metadata/%s/tool" sizeInBytes="%d" sha256="%s" senderUid="ANDROID-0000000000000000" '
            'senderCallsign="SENDER" name="Recon1"/>'
            '<ackrequest uid="ee9bbb9f-d51f-46d3-ae03-2aae1e6a22a2" ackrequested="true" tag="Recon1"/>'
            '<marti><dest callsign="%s"/></marti></detail></event>'
            % (file_hash, len(DATA), file_hash, dest))


class FrameTests(unittest.TestCase):
    def test_a_request_round_trips(self):
        frame = tak_files.encode_request(HASH)
        self.assertEqual(frame[0], tak_payload.FILE_REQUEST_V1)
        self.assertEqual(tak_files.decode_request(frame), HASH)

    def test_a_file_round_trips(self):
        frame = tak_files.encode_file(HASH, "Recon1.zip", DATA)
        self.assertEqual(tak_payload.name_of(frame), "file-v1")
        self.assertEqual(tak_files.decode_file(frame), (HASH, "Recon1.zip", DATA))

    def test_a_file_that_is_not_what_its_hash_names_is_refused(self):
        frame = bytearray(tak_files.encode_file(HASH, "Recon1.zip", DATA))
        frame[-1] ^= 0xFF
        self.assertIsNone(tak_files.decode_file(bytes(frame)))

    def test_other_frames_are_not_requests(self):
        self.assertIsNone(tak_files.decode_request(b"\x05" + bytes(32)))
        self.assertIsNone(tak_files.decode_request(b"\x06short"))


class NoticeTests(unittest.TestCase):
    def test_atak_s_notice_is_read(self):
        found = tak_files.parse_notice(notice())
        self.assertEqual(found["hash"], HASH)
        self.assertEqual(found["filename"], "Recon1.zip")
        self.assertEqual(found["size"], len(DATA))

    def test_anything_else_is_not_a_notice(self):
        self.assertIsNone(tak_files.parse_notice(notice().replace("b-f-t-r", "u-d-f")))
        self.assertIsNone(tak_files.parse_notice(notice(file_hash="nothex")))

    def test_the_rewrite_points_here_and_outlives_a_fetch(self):
        """ATAK stamps the notice stale ten seconds out, which is gone before
        a mesh fetch finishes; and its URL names the sender's host."""
        url = tak_files.content_url("http://127.0.0.1:18080", HASH)
        rewritten = tak_files.rewrite_notice(notice(), url)
        self.assertIn('senderUrl="%s"' % url, rewritten)
        self.assertNotIn("192.168.240.1", rewritten)
        found = tak_files.parse_notice(rewritten)
        self.assertEqual(found["hash"], HASH)
        self.assertNotIn('stale="2026-09-22T10:39:42.087Z"', rewritten)


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.store = tak_files.FileStore(self.folder.name)

    def tearDown(self):
        self.folder.cleanup()

    def test_bytes_are_kept_under_their_own_hash_owner_only(self):
        self.assertEqual(self.store.put(DATA, "Recon1.zip"), HASH)
        self.assertEqual(self.store.read(HASH), DATA)
        self.assertEqual(os.stat(os.path.join(self.folder.name, HASH)).st_mode & 0o777, 0o600)

    def test_bytes_that_are_not_the_expected_file_are_refused(self):
        self.assertIsNone(self.store.put(DATA + b"x", expected_hash=HASH))
        self.assertFalse(self.store.has(HASH))

    def test_only_an_addressee_may_fetch(self):
        self.store.put(DATA)
        self.store.grant(HASH, [ALPHA])
        member = lambda peer: True
        self.assertTrue(self.store.may_fetch(HASH, ALPHA, member))
        self.assertFalse(self.store.may_fetch(HASH, BRAVO, member))

    def test_a_file_offered_to_the_team_goes_to_members_only(self):
        self.store.put(DATA)
        self.store.grant(HASH, None)
        self.assertTrue(self.store.may_fetch(HASH, BRAVO, lambda peer: peer == BRAVO))
        self.assertFalse(self.store.may_fetch(HASH, ALPHA, lambda peer: peer == BRAVO))

    def test_a_file_offered_to_nobody_goes_to_nobody(self):
        self.store.put(DATA)
        self.assertFalse(self.store.may_fetch(HASH, ALPHA, lambda peer: True))


class ServiceTests(unittest.TestCase):
    """ATAK's four calls, over real HTTP on loopback."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.store = tak_files.FileStore(self.folder.name)
        self.uploaded = []
        self.service = tak_file_service.FileService(
            self.store, "https://192.168.240.1:8443", port=0,
            on_upload=self.uploaded.append, log=lambda *a: None)
        self.service.start()
        self.base = "http://127.0.0.1:%d" % self.service.port

    def tearDown(self):
        self.service.stop()
        self.folder.cleanup()

    def call(self, method, path, body=None, headers=None):
        request = urllib.request.Request(self.base + path, data=body, method=method,
                                         headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                return reply.status, reply.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def upload(self, data, file_hash=HASH):
        boundary = "atakboundary"
        body = ("--%s\r\nContent-Disposition: form-data; name=\"assetfile\"; filename=\"Recon1.zip\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n" % boundary).encode() + data + \
            ("\r\n--%s--\r\n" % boundary).encode()
        return self.call("POST", "/Marti/sync/missionupload?hash=%s&filename=Recon1.zip"
                         "&creatorUid=ANDROID-0000000000000000" % file_hash, body,
                         {"Content-Type": "multipart/form-data; boundary=%s" % boundary})

    def test_atak_s_sequence(self):
        self.assertEqual(self.call("GET", "/Marti/sync/missionquery?hash=%s" % HASH)[0], 404)

        status, body = self.upload(DATA)
        self.assertEqual(status, 200)
        self.assertIn(HASH.encode(), body)
        self.assertEqual(self.uploaded, [HASH])

        self.assertEqual(self.call("PUT", "/Marti/api/sync/metadata/%s/tool" % HASH, b"private")[0], 200)
        self.assertEqual(self.call("GET", "/Marti/sync/missionquery?hash=%s" % HASH)[0], 200)
        self.assertEqual(self.call("GET", "/Marti/sync/content?hash=%s" % HASH), (200, DATA))

    def test_an_upload_that_is_not_its_hash_is_refused(self):
        self.assertEqual(self.upload(DATA + b"tampered")[0], 400)
        self.assertFalse(self.store.has(HASH))


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        made = CotBridge.__new__(CotBridge)
        made.unreadable = 0
        made.files = tak_files.FileStore(self.folder.name)
        made.registry = Mock()
        made.registry.members.return_value = [ALPHA, BRAVO]
        made.registry.describe.return_value = {"callsign": "PEER"}
        made.rns = Mock()
        made.rns.Identity.recall.side_effect = lambda member: member
        made.lxmf = Mock()
        self.made = made

    def tearDown(self):
        self.folder.cleanup()

    def run_quietly(self, action, *args):
        with redirect_stdout(io.StringIO()):
            action(*args)

    def test_the_addressee_gets_the_file(self):
        self.made.files.put(DATA, "Recon1.zip")
        self.run_quietly(self.made._grant_file, notice(), [ALPHA])
        self.run_quietly(self.made._file_requested, tak_files.encode_request(HASH), ALPHA)

        self.made.lxmf.send_file.assert_called_once()
        identity, frame = self.made.lxmf.send_file.call_args[0]
        self.assertEqual(identity, ALPHA)
        self.assertEqual(tak_files.decode_file(frame), (HASH, "Recon1.zip", DATA))

    def test_a_member_who_was_not_offered_it_is_refused(self):
        self.made.files.put(DATA, "Recon1.zip")
        self.run_quietly(self.made._grant_file, notice(), [ALPHA])
        self.run_quietly(self.made._file_requested, tak_files.encode_request(HASH), BRAVO)
        self.made.lxmf.send_file.assert_not_called()

    def test_a_sender_is_told_when_an_offer_goes_unfetched(self):
        self.made.uid = "urtn-" + "aa" * 16
        self.made.drawn = []
        self.made._to_clients = lambda payload=None, keep=False: self.made.drawn.append(payload)
        self.made.files.put(DATA, "Recon1.zip")
        with unittest.mock.patch("threading.Timer"):
            self.run_quietly(self.made._grant_file, notice(), [ALPHA])
        self.made._offer_unfetched((HASH, ALPHA))
        self.made._offer_unfetched((HASH, ALPHA))
        self.assertEqual(len(self.made.drawn), 1)
        self.assertIn("not fetched yet by PEER", self.made.drawn[0].decode())

    def test_a_fetched_offer_says_nothing(self):
        self.made.uid = "urtn-" + "aa" * 16
        self.made.drawn = []
        self.made._to_clients = lambda payload=None, keep=False: self.made.drawn.append(payload)
        self.made.files.put(DATA, "Recon1.zip")
        with unittest.mock.patch("threading.Timer"):
            self.run_quietly(self.made._grant_file, notice(), [ALPHA])
        self.run_quietly(self.made._file_requested, tak_files.encode_request(HASH), ALPHA)
        self.made._offer_unfetched((HASH, ALPHA))
        self.assertEqual(self.made.drawn, [])

    def test_an_unproved_request_is_refused(self):
        self.made.files.put(DATA, "Recon1.zip")
        self.run_quietly(self.made._grant_file, notice(), None)
        self.run_quietly(self.made._file_requested, tak_files.encode_request(HASH), None)
        self.made.lxmf.send_file.assert_not_called()
        self.assertEqual(self.made.unreadable, 1)



class DeckReceivesTests(unittest.TestCase):
    """A file a handset's ATAK sent to the deck: fetched over a fast path,
    deferred over a slow one, and offered to Waydroid's ATAK only once here."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        made = CotBridge.__new__(CotBridge)
        made.bind_host = "192.168.240.1"
        made.received = 0
        made.registry = Mock()
        made.registry.describe.return_value = {"callsign": "LEXUS"}
        made.files = tak_files.FileStore(self.folder.name)
        made.rns = Mock()
        made.rns.Identity.recall.side_effect = lambda member: member
        made.lxmf = Mock()
        made.drawn = []
        made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
        self.rtt = 0.008
        made._measure_path = lambda member, done: done(self.rtt)
        self.made = made

    def tearDown(self):
        self.folder.cleanup()

    def offered(self, sender=ALPHA):
        with redirect_stdout(io.StringIO()):
            return self.made._file_offered_here(notice(dest="DECK"), sender)

    def arrives(self, sender=ALPHA, frame=None):
        with redirect_stdout(io.StringIO()):
            self.made._file_arrived(frame or tak_files.encode_file(HASH, "Recon1.zip", DATA), sender)

    def test_over_a_fast_path_it_is_fetched_then_offered(self):
        self.assertTrue(self.offered())
        self.made.lxmf.send_request.assert_called_once_with(ALPHA, tak_files.encode_request(HASH))
        self.assertEqual(self.made.drawn, [], "ATAK is not offered what it cannot fetch yet")

        self.arrives()
        self.assertEqual(self.made.files.read(HASH), DATA)
        self.assertEqual(len(self.made.drawn), 1)
        self.assertIn(b"http://192.168.240.1:8080/Marti/sync/content?hash=" + HASH.encode(),
                      self.made.drawn[0])

    def test_over_a_slow_path_it_waits(self):
        self.rtt = 4.9
        with unittest.mock.patch("threading.Timer"):
            self.offered()
        self.made.lxmf.send_request.assert_not_called()
        self.assertEqual(self.made.drawn, [])

    def test_a_file_from_someone_other_than_its_sender_is_discarded(self):
        self.offered(sender=ALPHA)
        self.arrives(sender=BRAVO)
        self.assertFalse(self.made.files.has(HASH))
        self.assertEqual(self.made.drawn, [])

    def test_a_file_nobody_asked_for_is_discarded(self):
        self.arrives()
        self.assertFalse(self.made.files.has(HASH))

    def test_a_file_already_held_is_offered_at_once(self):
        self.made.files.put(DATA, "Recon1.zip")
        self.offered()
        self.made.lxmf.send_request.assert_not_called()
        self.assertEqual(len(self.made.drawn), 1)

    def test_a_deferred_file_is_announced_once_from_columba(self):
        self.rtt = 4.9
        self.made.uid = "urtn-" + "aa" * 16
        self.made.registry = Mock()
        self.made.registry.describe.return_value = {"callsign": "LEXUS"}
        with unittest.mock.patch("threading.Timer"):
            self.offered()
            entry = self.made._pending_files()[HASH]
            self.made._attempt_file(HASH, entry)
        self.assertEqual(len(self.made.drawn), 1, "one line, not one per retry")
        line = self.made.drawn[0].decode()
        self.assertIn("from LEXUS is waiting", line)
        self.assertIn(tak_files.STATUS_UID, line)

    def test_files_waiting_on_one_sender_share_one_probe(self):
        self.rtt = 4.9
        probes = []
        self.made._measure_path = lambda member, done: probes.append(member) or done(self.rtt)
        other = hashlib.sha256(b"another").hexdigest()
        with unittest.mock.patch("threading.Timer"), redirect_stdout(io.StringIO()):
            self.made._file_offered_here(notice(dest="DECK"), ALPHA)
            self.made._file_offered_here(notice(file_hash=other, dest="DECK"), ALPHA)
        self.assertEqual(len(probes), 1)

    def test_anything_else_passes_through(self):
        with redirect_stdout(io.StringIO()):
            self.assertFalse(self.made._file_offered_here(notice().replace("b-f-t-r", "u-d-f"), ALPHA))


if __name__ == "__main__":
    unittest.main()
