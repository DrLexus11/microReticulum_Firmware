"""Values that look valid, decode cleanly, and leave the bridge broken.

Each of these starts a bridge that reports itself healthy and is not. That is
the shape worth testing: an argument that fails loudly is found in a second,
and one that fails silently is found by somebody out of range.
"""

import subprocess
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
BRIDGE = TOOLS / "cot_bridge.py"


def run(args, secret="x" * 32):
    """The bridge's argument handling, without a Reticulum instance.

    Arguments are validated before anything is opened, so a rejected value
    exits before the parts that would need hardware.
    """
    return subprocess.run(
        [sys.executable, str(BRIDGE)] + args,
        capture_output=True, text=True, timeout=60,
        env={"PATH": "/usr/bin:/bin", "TAK_FLEET_SECRET": secret,
             "HOME": "/nonexistent-home-for-this-test"},
    )


class AnnounceIntervalTests(unittest.TestCase):
    """type=int accepts 0 and negatives, and both break announcing in a way
    the bridge does not notice."""

    def test_zero_is_refused(self):
        """time.sleep(0) turns the announce loop into a spin. The result is not
        a fast node: a transport node rate-limits a destination that announces
        that hard, so announcing too much ends in not being heard at all."""
        result = run(["--announce-interval", "0"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("positive", result.stderr + result.stdout)

    def test_a_negative_is_refused(self):
        """It raises inside a daemon thread, where nothing is watching.
        Periodic announces stop and the bridge goes on reporting itself fine."""
        result = run(["--announce-interval", "-5"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("positive", result.stderr + result.stdout)


class PropagationNodeTests(unittest.TestCase):
    def test_hex_that_is_not_a_destination_hash_is_refused(self):
        """fromhex() takes any even-length string, so this gets as far as the
        router. The bridge then starts, reports a propagation node, and holds
        nothing for anybody -- discovered only when somebody walks out of range
        having been told they were covered."""
        result = run(["--propagation-node", "deadbeef"])
        self.assertNotEqual(result.returncode, 0)
        output = result.stderr + result.stdout
        self.assertIn("16 bytes", output)

    def test_not_hex_at_all_is_refused(self):
        result = run(["--propagation-node", "nothexatall"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hex", result.stderr + result.stdout)


class OptionalLxmfTests(unittest.TestCase):
    def test_the_module_imports_without_lxmf(self):
        """--no-lxmf documents bare-packet mode as a way to run, and
        test_tak_lxmf.py already treats LXMF as optional -- but a top-level
        import made the module unimportable without it, so the mode that exists
        to survive LXMF's absence could not start without LXMF installed."""
        script = (
            "import sys, types\n"
            # Make any import of LXMF fail, the way an uninstalled package does.
            "class Blocker:\n"
            "    def find_module(self, name, path=None):\n"
            "        return self if name in ('LXMF', 'tak_lxmf') else None\n"
            "    def load_module(self, name):\n"
            "        raise ImportError(name)\n"
            "sys.meta_path.insert(0, Blocker())\n"
            "sys.path.insert(0, %r)\n"
            "import cot_bridge\n"
            "print('imported', cot_bridge.DEFAULT_PORT)\n" % str(TOOLS)
        )
        result = subprocess.run([sys.executable, "-c", script],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("imported", result.stdout)


if __name__ == "__main__":
    unittest.main()
