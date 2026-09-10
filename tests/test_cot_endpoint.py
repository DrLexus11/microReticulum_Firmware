"""Framing CoT out of a TCP stream, which is where CoT is usually lost."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_endpoint import CotStream, is_self_addressed

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


if __name__ == "__main__":
    unittest.main()
