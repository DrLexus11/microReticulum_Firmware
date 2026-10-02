"""The device page's "boot" category: the fields a soak reads over the mesh.

PR E2 decided that Rev 1 ran 24 h without a restart from this page alone --
uptime, reset reason, crash and panic totals -- because a board on a wall plug
has no serial port to watch. A dropped field or broken JSON would silently
invalidate such a reading, so the block in Pages.h is pinned here, and rendered
with sample values to prove its output parses.
"""

import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = ROOT / "Pages.h"

FIELDS = ["uptime_s", "reset_reason", "prev_uptime_s", "boots_since_power", "crashes", "panics"]


def boot_block():
    source = PAGES.read_text()
    start = source.index('category == "boot"')
    # Up to whichever category follows; the order of categories is not fixed.
    end = source.index('else if (category == ', start + 1)
    return source[start:end]


def render(block, rail_lost):
    """The page's text for sample values, built from the block's own lines."""
    out = []
    for line in block.splitlines():
        line = line.strip()
        if not line.startswith("content"):
            continue
        if line.startswith('content = "{'):
            out.append("{\n")
            continue
        if line.startswith('content << "}"'):
            out.append("}")
            continue
        line = re.sub(r"\(boot_rail_lost \? std::string\(\"null\"\) : std::to_string\(boot_prev_uptime\)\)",
                      '"null"' if rail_lost else '"321"', line)
        line = re.sub(r"std::to_string\([^)]*\)", '"7"', line)
        line = line.replace("boot_reset_reason", '"TASK_WDT"')
        pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', line)
        out.append("".join(bytes(p, "utf-8").decode("unicode_escape") for p in pieces))
    return "".join(out)


class BootPage(unittest.TestCase):

    def test_every_field_is_served_in_order(self):
        block = boot_block()
        positions = [block.index('\\"%s\\"' % field) for field in FIELDS]
        self.assertEqual(positions, sorted(positions))

    def test_previous_run_is_null_after_a_lost_rail(self):
        self.assertIn('boot_rail_lost ? std::string("null")', boot_block())

    def test_it_renders_as_json_with_and_without_a_previous_run(self):
        for rail_lost in (True, False):
            page = json.loads(render(boot_block(), rail_lost))
            self.assertEqual(list(page), FIELDS)
            self.assertEqual(page["reset_reason"], "TASK_WDT")
            self.assertEqual(page["prev_uptime_s"], None if rail_lost else 321)


class NodeHashArguments(unittest.TestCase):
    """node_page and link_churn reject a bad hash before Reticulum starts."""

    def run_tool(self, *argv):
        return subprocess.run([sys.executable, *argv], cwd=ROOT, capture_output=True, text=True, timeout=60)

    def test_bad_hex_and_wrong_length_are_refused(self):
        for argv in (["tools/node_page.py", "zz", "/page/x.mu"],
                     ["tools/node_page.py", "abcd", "/page/x.mu"],
                     ["tools/link_churn.py", "--to", "zz"],
                     ["tools/link_churn.py", "--to", "abcd"]):
            result = self.run_tool(*argv)
            self.assertEqual(result.returncode, 2, (argv, result.stderr))
            self.assertRegex(result.stderr, r"not hex|a destination hash is 16")


if __name__ == "__main__":
    unittest.main()
