"""Framing CoT out of a TCP stream, which is where CoT is usually lost."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_endpoint import CotStream, is_self_addressed, learn_atak_uid, rewrite_self_uid

A = b'<event uid="a" type="a-f-G"><point lat="1" lon="2"/><detail/></event>'
B = b'<event uid="b" type="a-h-G"><point lat="3" lon="4"/><detail/></event>'


class StreamTests(unittest.TestCase):
    def setUp(self):
        self.stream = CotStream()

    def test_one_event_in_one_segment(self):
        self.assertEqual(self.stream.feed(A), [A])

    def test_several_events_in_one_segment(self):
        self.assertEqual(self.stream.feed(A + B), [A, B])

    def test_an_event_split_across_every_possible_boundary(self):
        """A read() is not a message. Splitting mid-attribute is the case that
        breaks naive readers, and TCP will produce it under load."""
        for cut in range(1, len(A)):
            stream = CotStream()
            first = stream.feed(A[:cut])
            second = stream.feed(A[cut:])
            self.assertEqual(first + second, [A], "split at %d" % cut)

    def test_a_byte_at_a_time(self):
        got = []
        for index in range(len(A + B)):
            got.extend(self.stream.feed((A + B)[index:index + 1]))
        self.assertEqual(got, [A, B])

    def test_whitespace_and_declarations_between_documents_are_ignored(self):
        noisy = b'<?xml version="1.0"?>\n' + A + b"\r\n  " + B
        self.assertEqual(self.stream.feed(noisy), [A, B])

    def test_nothing_is_emitted_until_an_event_closes(self):
        self.assertEqual(self.stream.feed(A[:-1]), [])
        self.assertEqual(self.stream.feed(A[-1:]), [A])

    def test_noise_without_an_event_does_not_accumulate(self):
        for _ in range(500):
            self.stream.feed(b"garbage that is not cot at all")
        self.assertLessEqual(self.stream.pending, len(b"<event"))

    def test_a_runaway_event_is_abandoned_not_the_connection(self):
        """One peer sending a document that never ends must not cost us the
        events that follow it."""
        stream = CotStream(max_event_bytes=1024)
        stream.feed(b'<event uid="huge" ' + b"x" * 4096)
        self.assertEqual(stream.pending, 0)
        self.assertEqual(stream.feed(B), [B])


class EchoTests(unittest.TestCase):
    def test_our_own_event_is_recognised(self):
        own = b'<event uid="urtn-' + b"00" * 16 + b'" type="a-f-G-U-C"><detail/></event>'
        self.assertTrue(is_self_addressed(own, "urtn-" + "00" * 16))
        self.assertFalse(is_self_addressed(A, "urtn-" + "00" * 16))

    def test_a_uid_appearing_only_in_the_detail_is_not_us(self):
        """A marker's <creator uid="..."> names whoever dropped it. Treating
        that as self-addressed would silently stop forwarding their markers."""
        theirs = (b'<event uid="marker-1" type="a-h-G"><detail>'
                  b'<creator uid="urtn-' + b"00" * 16 + b'"/></detail></event>')
        self.assertFalse(is_self_addressed(theirs, "urtn-" + "00" * 16))

    def test_no_uid_configured_means_nothing_is_self_addressed(self):
        self.assertFalse(is_self_addressed(A, ""))
        self.assertFalse(is_self_addressed(A, None))

    def test_an_angle_bracket_in_an_earlier_attribute_does_not_hide_the_uid(self):
        """A closing angle bracket is legal unescaped in an XML attribute
        value. A scan that stopped at the first one would miss the uid and
        forward our own event straight back into the mesh."""
        ours = "urtn-" + "ab" * 16
        awkward = ('<event how="a>b" uid="%s" type="a-f-G-U-C"><detail/></event>' % ours)
        self.assertTrue(is_self_addressed(awkward.encode("utf-8"), ours))

    def test_an_attribute_whose_name_ends_in_uid_is_not_the_uid(self):
        """A substring check would read somebody else's event as our own and
        quietly stop forwarding it."""
        ours = "urtn-" + "ab" * 16
        nested = ('<event parent_uid="%s" uid="other" type="a-f-G"><detail/></event>' % ours)
        self.assertFalse(is_self_addressed(nested.encode("utf-8"), ours))




PLI = ('<event uid="ANDROID-7819dadfcf858641" type="a-f-G-U-C" how="m-g" version="2.0">'
       '<point lat="40.95" lon="29.09" hae="33.5" ce="44.0" le="9999999.0"/>'
       '<detail><takv device="SAMSUNG SM-A546E" os="36" platform="ATAK-CIV" version="5.6"/>'
       '<contact callsign="LEXUS" endpoint="*:-1:stcp"/><uid Droid="LEXUS"/>'
       '<__group name="Cyan" role="Team Member"/></detail></event>')

MARKER = ('<event uid="66d6de40-bd62-4a5e-92ef-d4ee14b0194a" type="a-h-G" how="h-g-i-g-o" '
          'version="2.0"><point lat="40.954" lon="29.094" hae="48.0" ce="9999999.0" '
          'le="9999999.0"/><detail><contact callsign="R.10.144053"/>'
          '<creator callsign="LEXUS" uid="ANDROID-7819dadfcf858641"/></detail></event>')

OURS = "urtn-" + "ab" * 16


class UidRewriteTests(unittest.TestCase):
    def test_our_self_report_gets_a_reticulum_rooted_uid(self):
        """Otherwise the derived identity exists only in the codebase and
        never reaches a track anybody looks at."""
        import xml.etree.ElementTree as ET
        out = rewrite_self_uid(PLI, "ANDROID-7819dadfcf858641", OURS)
        self.assertEqual(ET.fromstring(out).get("uid"), OURS)
        # The rest of the event is untouched.
        self.assertEqual(ET.fromstring(out).find("detail/contact").get("callsign"), "LEXUS")

    def test_a_marker_keeps_its_own_uid(self):
        """A marker is a distinct object that happens to have been created
        here. Rewriting them would collapse every marker this node ever
        dropped into a single track."""
        import xml.etree.ElementTree as ET
        out = rewrite_self_uid(MARKER, "ANDROID-7819dadfcf858641", OURS)
        self.assertEqual(ET.fromstring(out).get("uid"), "66d6de40-bd62-4a5e-92ef-d4ee14b0194a")

    def test_a_creator_uid_in_the_detail_does_not_trigger_a_rewrite(self):
        """The marker above names our device as its creator. Matching on that
        would rewrite every marker we drop."""
        self.assertIn("66d6de40", rewrite_self_uid(MARKER, "ANDROID-7819dadfcf858641", OURS))

    def test_nothing_is_rewritten_before_the_atak_uid_is_known(self):
        for atak_uid, ours in (("", OURS), (None, OURS), ("ANDROID-x", ""), ("ANDROID-x", None)):
            self.assertEqual(rewrite_self_uid(PLI, atak_uid, ours), PLI)


class LearnUidTests(unittest.TestCase):
    def test_the_atak_uid_is_learned_from_a_self_report(self):
        """Nothing configures it: ATAK announces it in every position report,
        and a setting an operator must type is a setting that can be wrong."""
        self.assertEqual(learn_atak_uid(PLI), "ANDROID-7819dadfcf858641")

    def test_a_marker_never_teaches_us_a_device_uid(self):
        self.assertIsNone(learn_atak_uid(MARKER))

    def test_malformed_input_is_not_an_error(self):
        """ParseError descends from SyntaxError, not ValueError, so this is
        the case a caller's obvious `except ValueError` would have missed."""
        for bad in (b"", b"<event", b"not xml at all", b"<other/>",
                    b'<!DOCTYPE event [<!ENTITY x "boom">]><event uid="a"/>'):
            self.assertIsNone(learn_atak_uid(bad), repr(bad))
if __name__ == "__main__":
    unittest.main()
