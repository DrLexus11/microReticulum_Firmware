"""Compile and run the actual ESPNowInterface with deterministic radio fakes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ESPNowRuntimeTests(unittest.TestCase):
    def test_recovery_preserves_forwarding_and_reaches_sibling_peers(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "mesh-test")
            compile_result = subprocess.run([
                *shlex.split(os.environ.get("CXX", "g++")),
                *shlex.split(os.environ.get("CXXFLAGS", "")),
                "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "tests/espnow_host"), "-I" + str(ROOT),
                str(ROOT / "tests/espnow_host/mesh_test.cpp"), "-o", binary,
            ], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([binary], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
