"""PR D2 review notes, one test class each -- the regressions they guard.

Each is a way a crafted or unlucky frame from the mesh could reach something
it should not: a response header, the wrong XML attribute, an unbounded fetch,
a late part, or a flood of path requests.
"""

import hashlib
import io
import sys
import tempfile
import unittest
import unittest.mock
import xml.etree.ElementTree as ET
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_file_service                  # noqa: E402
import tak_files                         # noqa: E402
from cot_bridge import CotBridge         # noqa: E402
from test_tak_files import (ALPHA, DATA, HASH, PartsTests, notice,   # noqa: E402
                            quickpic_package)


class HeaderInjectionTests(unittest.TestCase):
    """Note 1: a filename with CR/LF must not become another response header."""

    def test_control_characters_and_quotes_are_removed(self):
        header = tak_file_service.content_disposition('evil.zip"\r\nSet-Cookie: x=1')
        self.assertNotIn("\r", header)
        self.assertNotIn("\n", header)
        self.assertEqual(header.count('"'), 2)

    def test_a_name_beyond_ascii_is_carried_encoded(self):
        header = tak_file_service.content_disposition("keşif.zip")
        self.assertIn("filename*=UTF-8''ke%C5%9Fif.zip", header)
        header.encode("latin-1")   # a header value must be encodable


class RewriteScopeTests(unittest.TestCase):
    """Note 2: only fileshare's senderUrl is rewritten, escaped, literally."""

    def test_an_earlier_sender_url_elsewhere_is_left_alone(self):
        crafted = notice().replace("<detail>", '<detail><link senderUrl="https://decoy/"/>')
        out = tak_files.rewrite_notice(crafted, "http://127.0.0.1:8080/x")
        self.assertIn('<link senderUrl="https://decoy/"/>', out)
        share = ET.fromstring(out).find("detail/fileshare")
        self.assertEqual(share.get("senderUrl"), "http://127.0.0.1:8080/x")

    def test_a_name_inside_another_value_is_not_the_attribute(self):
        crafted = notice().replace('filename="Recon1.zip"', "filename=\"a senderUrl='x'.zip\"")
        out = tak_files.rewrite_notice(crafted, "http://h/y")
        share = ET.fromstring(out).find("detail/fileshare")
        self.assertEqual(share.get("filename"), "a senderUrl='x'.zip")
        self.assertEqual(share.get("senderUrl"), "http://h/y")

    def test_the_url_is_escaped_and_taken_literally(self):
        out = tak_files.rewrite_notice(notice(), "http://h/c?a=1&b=\\1")
        self.assertEqual(ET.fromstring(out).find("detail/fileshare").get("senderUrl"), "http://h/c?a=1&b=\\1")


class PathRequestBackoffTests(unittest.TestCase):
    """Notes 3 and 5: every per-send path request goes through the backoff."""

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.unreachable = 0
        made.rns = Mock()
        made.rns.Identity.recall.return_value = None
        made.registry = Mock()
        made.registry.members.return_value = [ALPHA]
        made.lxmf = Mock()
        return made

    def test_reliable_fan_out_to_an_unrecalled_member_is_backed_off(self):
        made = self.bridge()
        for _ in range(10):
            made._fan_out_reliably(b"\x05frame")
        self.assertEqual(made.rns.Transport.request_path.call_count, 1)
        self.assertEqual(made.unreachable, 10)

    def test_a_file_probe_to_an_unrecalled_sender_is_backed_off(self):
        made = self.bridge()
        results = []
        for _ in range(10):
            made._measure_path(ALPHA, results.append)
        self.assertEqual(results, [None] * 10)
        self.assertEqual(made.rns.Transport.request_path.call_count, 1)


class OutstandingPartTests(PartsTests):
    """Note 4: only the part asked for, while still waited for, is kept."""

    def arrive(self, offset, data, total=None):
        with redirect_stdout(io.StringIO()):
            self.made._part_arrived(tak_files.encode_part(
                self.BIG_HASH, offset, len(self.BIG) if total is None else total, data), ALPHA)

    def test_a_late_part_after_a_stall_is_discarded(self):
        entry = self.made._pending_files()[self.BIG_HASH]
        with redirect_stdout(io.StringIO()):
            self.made._part_stalled(self.BIG_HASH, entry, entry["asked"])
        self.arrive(0, self.BIG[:tak_files.FIRST_PART_BYTES])
        self.assertEqual(self.made.files.partial_size(self.BIG_HASH), 0)

    def test_a_part_of_another_length_is_discarded(self):
        self.arrive(0, self.BIG[:1000])
        self.assertEqual(self.made.files.partial_size(self.BIG_HASH), 0)

    def test_a_part_claiming_another_size_is_discarded(self):
        self.arrive(0, self.BIG[:tak_files.FIRST_PART_BYTES], total=len(self.BIG) * 4)
        self.assertEqual(self.made.files.partial_size(self.BIG_HASH), 0)


class NoticeSizeTests(unittest.TestCase):
    """Note 6: a size nothing here would store is not an offer."""

    def test_sizes_outside_the_limit_are_refused(self):
        for size in (-1, 0, tak_files.MAX_FILE_BYTES + 1):
            crafted = notice().replace('sizeInBytes="%d"' % len(DATA), 'sizeInBytes="%d"' % size)
            self.assertIsNone(tak_files.parse_notice(crafted), size)
        self.assertIsNone(tak_files.parse_notice(notice().replace(
            'sizeInBytes="%d"' % len(DATA), 'sizeInBytes="lots"')))


class OfferBoundsTests(unittest.TestCase):
    """Note 7: no offer over three fragments, and no unknown flags."""

    def test_an_offer_over_the_bound_is_refused(self):
        frame = tak_files.encode_offer(1, HASH, 10, "a.zip", thumbnail=b"x" * tak_files.MAX_OFFER_BYTES)
        self.assertIsNone(tak_files.decode_offer(frame))

    def test_unknown_flags_are_refused(self):
        frame = bytearray(tak_files.encode_offer(1, HASH, 10, "a.zip"))
        frame[41] |= 0x80
        self.assertIsNone(tak_files.decode_offer(bytes(frame)))

    def test_an_offer_for_an_impossible_size_is_refused(self):
        self.assertIsNone(tak_files.decode_offer(
            tak_files.encode_offer(1, HASH, tak_files.MAX_FILE_BYTES + 1, "a.zip")))


class PreviewEscapingTests(unittest.TestCase):
    """Note 8: a callsign with XML metacharacters still makes valid XML."""

    def test_the_preview_marker_parses(self):
        package = quickpic_package()
        offer = tak_files.decode_offer(tak_files.offer_for(
            7, hashlib.sha256(package).hexdigest(), "R&D <1>.jpg.zip", package))
        preview, _ = tak_files.preview_package(offer, "A&B <x> 'y'")
        with zipfile.ZipFile(io.BytesIO(preview)) as zipped:
            marker = [n for n in zipped.namelist() if n.endswith(".cot")][0]
            event = ET.fromstring(zipped.read(marker))
            ET.fromstring(zipped.read("MANIFEST/manifest.xml"))
        self.assertEqual(event.find("detail/contact").get("callsign"), "A&B <x> 'y' preview")


if __name__ == "__main__":
    unittest.main()
