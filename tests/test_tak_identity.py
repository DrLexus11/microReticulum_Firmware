"""EUD identity derived from a Reticulum destination, and announced claims."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_identity as identity


class UidTests(unittest.TestCase):
    def setUp(self):
        self.destination = bytes(range(16))

    def test_uid_round_trips_and_is_stable(self):
        uid = identity.uid_for(self.destination)
        self.assertEqual(uid, "urtn-" + self.destination.hex())
        self.assertEqual(identity.destination_for(uid), self.destination)
        # Stability is the whole point of the pivot: same destination, same UID,
        # for the life of the identity and across restarts and re-provisions.
        self.assertEqual(identity.uid_for(self.destination), uid)

    def test_the_full_hash_is_carried_not_a_truncation(self):
        """The defect this replaces was a 32-bit truncation of a 128-bit hash.

        Two destinations sharing their first four bytes had the same UID and
        therefore the same ATAK track.
        """
        other = bytes([0, 1, 2, 3]) + bytes(range(200, 212))
        self.assertEqual(self.destination[:4], other[:4])
        self.assertNotEqual(identity.uid_for(self.destination), identity.uid_for(other))

    def test_foreign_uids_are_not_addressable(self):
        """A real ATAK network is full of UIDs we did not issue.

        Returning None rather than guessing is what stops a caller mistaking
        somebody else's marker for a peer it can send a task to.
        """
        for foreign in ("ANDROID-7819dadfcf858641",
                        "66d6de40-bd62-4a5e-92ef-d4ee14b0194a",
                        "urtn-tooshort",
                        "urtn-" + "zz" * 16,
                        "", None, 42):
            self.assertIsNone(identity.destination_for(foreign), repr(foreign))

    def test_a_wrong_length_hash_is_refused(self):
        for bad in (b"", bytes(15), bytes(17)):
            with self.assertRaises(ValueError):
                identity.uid_for(bad)


class AnnounceTests(unittest.TestCase):
    def test_callsign_and_team_round_trip(self):
        payload = identity.announce_payload("RAD-BE13B3", "Cyan", "Team Member")
        self.assertEqual(identity.parse_announce(payload),
                         {"callsign": "RAD-BE13B3", "team": "Cyan", "role": "Team Member"})

    def test_non_ascii_callsign_survives(self):
        payload = identity.announce_payload("Kılavuz-1", "Yellow", "HQ")
        self.assertEqual(identity.parse_announce(payload)["callsign"], "Kılavuz-1")

    def test_foreign_or_damaged_app_data_is_not_an_error(self):
        """Announces arrive from every node on the mesh, ours or not."""
        for payload in (None, b"", b"\x00", b"\x99\x01\x01abc",
                        bytes([identity.ANNOUNCE_VERSION, 5, 1]) + b"ab",
                        bytes([identity.ANNOUNCE_VERSION, 1, 1]) + b"\xff\xfe\xfd"):
            self.assertIsNone(identity.parse_announce(payload), repr(payload))

    def test_empty_and_oversized_and_control_characters_are_refused(self):
        for callsign in ("", "x" * (identity.MAX_CALLSIGN + 1), "bad\ncallsign", "bell\x07"):
            with self.assertRaises(ValueError):
                identity.announce_payload(callsign)

    def test_payload_stays_small_enough_to_announce(self):
        # app_data rides in every announce, so it is paid repeatedly.
        payload = identity.announce_payload("X" * identity.MAX_CALLSIGN,
                                            "Dark Green", "Team Member")
        self.assertLessEqual(len(payload), 96)


if __name__ == "__main__":
    unittest.main()
