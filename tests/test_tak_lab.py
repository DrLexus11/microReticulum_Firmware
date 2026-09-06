"""Exercise process lookup without starting or stopping any lab services."""

from pathlib import Path
import re
import subprocess
import unittest


SCRIPT = (Path(__file__).resolve().parents[1] / "tools/tak_lab.sh").read_text()


def function(name):
    return re.search(r"^" + name + r"\(\) \{(?:[^\n]*\}|.*?^\})$",
                     SCRIPT, re.MULTILINE | re.DOTALL).group(0)


class ProcessLookupTests(unittest.TestCase):
    def test_own_pid_is_filtered_but_other_pids_survive(self):
        result = subprocess.run(
            ["bash", "-c", function("pid_of") + '''
pgrep() { printf '%s\\n' "$$" "${$}0" 424242; }
pid_of /example/cot_gateway.py
printf 'expected=%s0\\n' "$$"
'''], capture_output=True, text=True, check=True)
        actual, expected = result.stdout.splitlines()
        self.assertEqual(actual, expected.removeprefix("expected="))

    def test_gateway_start_looks_up_the_full_path(self):
        result = subprocess.run(
            ["bash", "-c", function("start_gateway") + '''
REPO='/example/repo with spaces'
pid_of() { printf '%s\\n' "$1" >&2; printf '424242\\n'; }
say() { :; }
start_gateway simple
'''], capture_output=True, text=True, check=True)
        self.assertEqual(result.stderr.strip(),
                         "/example/repo with spaces/tools/cot_gateway.py")


if __name__ == "__main__":
    unittest.main()
