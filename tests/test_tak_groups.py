"""Teams as GROUP destinations, derived from a fleet secret."""

import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_groups as groups

try:
    import RNS
except ImportError:
    RNS = None

SECRET = b"a fleet secret long enough to pass"


class NormalisationTests(unittest.TestCase):
    def test_case_and_spacing_do_not_fork_a_team(self):
        """Two members whose keys disagree each broadcast and hear nothing."""
        canonical = groups.normalise_team("Dark Green")
        for variant in ("dark green", "  Dark   Green  ", "DARK GREEN", "Dark  Green"):
            self.assertEqual(groups.normalise_team(variant), canonical, variant)

    def test_empty_and_non_string_names_are_refused(self):
        for bad in ("", "   ", "\t\n", None, 7, b"Cyan"):
            with self.assertRaises(ValueError):
                groups.normalise_team(bad)


class KeyTests(unittest.TestCase):
    def test_key_is_stable_and_team_specific(self):
        key = groups.group_key("Cyan", SECRET)
        self.assertEqual(len(key), groups.GROUP_KEY_BYTES)
        self.assertEqual(groups.group_key(" cyan ", SECRET), key)
        self.assertNotEqual(groups.group_key("Green", SECRET), key)

    def test_a_different_fleet_secret_is_a_different_mesh(self):
        self.assertNotEqual(groups.group_key("Cyan", SECRET),
                            groups.group_key("Cyan", b"another secret entirely!!"))

    def test_the_team_name_never_appears_in_the_aspects(self):
        """A destination hash is visible to anyone who can hear an announce.

        If the aspects carried "cyan", a passive listener could enumerate the
        teams on the mesh and count each one's traffic without holding anything.
        """
        aspects = groups.group_aspects("Cyan", SECRET)
        self.assertNotIn("cyan", " ".join(aspects).lower())
        self.assertEqual(aspects[:2], groups.ASPECTS)


class SecretLoadingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = os.path.join(self.directory.name, "fleet-secret")

    def write(self, content, mode=0o600):
        Path(self.path).write_bytes(content)
        os.chmod(self.path, mode)

    def test_environment_wins_and_never_needs_a_command_line(self):
        secret = groups.load_fleet_secret(environment={groups.SECRET_ENVIRONMENT: "x" * 32})
        self.assertEqual(secret, b"x" * 32)

    def test_a_private_file_is_accepted_and_trailing_newline_ignored(self):
        self.write(SECRET + b"\n")
        self.assertEqual(groups.load_fleet_secret(self.path, environment={}), SECRET)

    def test_a_readable_by_others_secret_is_refused(self):
        """Refusing is the point: a secret others can read is not a secret,
        and silently using it would hide that from the operator."""
        self.write(SECRET, mode=0o644)
        with self.assertRaises(ValueError):
            groups.load_fleet_secret(self.path, environment={})

    def test_a_symlinked_secret_is_refused(self):
        target = os.path.join(self.directory.name, "elsewhere")
        Path(target).write_bytes(SECRET)
        os.chmod(target, 0o600)
        link = os.path.join(self.directory.name, "link")
        os.symlink(target, link)
        with self.assertRaises(ValueError):
            groups.load_fleet_secret(link, environment={})

    def test_a_short_secret_is_refused(self):
        self.write(b"tooshort")
        with self.assertRaises(ValueError):
            groups.load_fleet_secret(self.path, environment={})

    def test_a_missing_secret_says_how_to_supply_one(self):
        with self.assertRaises(ValueError) as caught:
            groups.load_fleet_secret(os.path.join(self.directory.name, "absent"), environment={})
        self.assertIn(groups.SECRET_ENVIRONMENT, str(caught.exception))


@unittest.skipUnless(RNS, "requires RNS virtualenv")
class DestinationTests(unittest.TestCase):
    def test_hash_matches_what_reticulum_derives(self):
        """The derivation is delegated, not copied; this proves it stayed so.

        A local reimplementation would be one upstream change away from
        addressing a team nobody else agrees with -- a team that transmits and
        is never heard.
        """
        aspects = groups.group_aspects("Cyan", SECRET)
        self.assertEqual(groups.group_destination_hash("Cyan", SECRET),
                         RNS.Destination.hash(None, groups.APP, *aspects))

    def test_teams_and_secrets_produce_distinct_destinations(self):
        seen = {
            groups.group_destination_hash("Cyan", SECRET),
            groups.group_destination_hash("Green", SECRET),
            groups.group_destination_hash("Cyan", b"another secret entirely!!"),
        }
        self.assertEqual(len(seen), 3)


if __name__ == "__main__":
    unittest.main()
