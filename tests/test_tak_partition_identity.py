"""The harness's peer identity is a private key on disk like any other.

It used to check `path.exists()` and then hand the pathname to RNS, which
reopens by name -- so a local process can swap the path between the two calls
and choose this peer's identity for it. It also wrote the key and relaxed the
mode afterwards, leaving a window at whatever the process umask allowed.

A test harness's key is still a key, and the project already had the answer in
cot_bridge.load_node_identity. The fix was to call it, not to copy it: a second
copy of a security pattern is a second place for it to rot.
"""

import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_bridge  # noqa: E402


class DelegationTests(unittest.TestCase):
    def test_the_harness_delegates_rather_than_repeating_the_pattern(self):
        source = (Path(__file__).resolve().parents[1]
                  / "tools" / "tak_partition_check.py").read_text()
        self.assertIn("cot_bridge.load_node_identity(", source)


class IdentityFileTests(unittest.TestCase):
    """What the harness now inherits, asserted where it lives."""

    def setUp(self):
        self.home = Path(tempfile.mkdtemp(prefix="tak-identity-"))
        self.addCleanup(shutil.rmtree, self.home, ignore_errors=True)
        self.path = self.home / "identity"

    def test_a_new_identity_is_private_from_the_moment_it_exists(self):
        """Created 0600 rather than written and then made private: the gap
        between those two is a window where the key is readable by anyone on
        the box."""
        cot_bridge.load_node_identity(self.path)
        mode = stat.S_IMODE(os.stat(self.path).st_mode)
        self.assertEqual(mode & (stat.S_IRWXG | stat.S_IRWXO), 0,
                         "identity is group- or world-accessible")

    def test_the_same_identity_comes_back(self):
        """The whole point of a persistent peer: returning is returning rather
        than arriving as somebody new."""
        first = cot_bridge.load_node_identity(self.path)
        second = cot_bridge.load_node_identity(self.path)
        self.assertEqual(first.hash, second.hash)

    def test_a_readable_identity_is_refused(self):
        """A key anyone can read is not one worth loading quietly."""
        cot_bridge.load_node_identity(self.path)
        os.chmod(self.path, 0o644)
        with self.assertRaises(ValueError):
            cot_bridge.load_node_identity(self.path)

    def test_a_symlink_is_refused_rather_than_followed(self):
        """O_NOFOLLOW is what stops a local process redirecting the read to a
        key of its choosing."""
        cot_bridge.load_node_identity(self.path)
        target = self.home / "elsewhere"
        self.path.rename(target)
        self.path.symlink_to(target)
        with self.assertRaises(ValueError):
            cot_bridge.load_node_identity(self.path)


if __name__ == "__main__":
    unittest.main()
