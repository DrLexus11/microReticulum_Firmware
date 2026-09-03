"""Access policy for the NomadNet diagnostic pages.

Reticulum offers ALLOW_NONE, ALLOW_ALL and ALLOW_LIST and nothing between, and
an ALLOW_LIST refusal is made inside Destination before the request handler
runs -- so the client receives no response at all. That is indistinguishable
from a node that is switched off, and it is how a browser repeatedly presented
a node that was in fact healthy and deliberately denying it.
"""

import os
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def live_source(name):
    """Source with commented-out lines removed.

    RNode_Firmware.ino keeps the previous ALLOW_LIST registrations commented
    beside the live ones as a record of what changed; matching those would make
    this test fail on history rather than on behaviour.
    """
    return "\n".join(line for line in source(name).splitlines()
                      if not line.lstrip().startswith("//"))


GATED = ("/page/stack.mu", "/page/device.mu", "/page/espnow.mu", "/page/ble.mu")
PUBLIC = ("/page/index.mu", "/page/time.mu")


class PageAccessPolicyTests(unittest.TestCase):
    def test_no_page_is_registered_allow_list(self):
        firmware = live_source("RNode_Firmware.ino")
        for page in GATED + PUBLIC:
            self.assertNotIn(
                'register_request_handler("%s", serve_page, '
                'RNS::Type::Destination::ALLOW_LIST' % page, firmware,
                "%s is refused before serve_page runs, which reaches the "
                "client as a silent timeout" % page)

    def test_telemetry_pages_are_gated_on_identity_in_the_handler(self):
        pages = source("Pages.h")
        gate = pages[pages.index("inline bool page_requires_identity"):
                     pages.index("void add_interface_details")]
        for page in GATED:
            self.assertIn('"%s"' % page, gate)
        for page in PUBLIC:
            self.assertNotIn('"%s"' % page, gate)
        self.assertIn("#ifdef NOMADNET_PAGES_ALLOW_ALL", gate)

    def test_an_unidentified_peer_is_answered_rather_than_dropped(self):
        pages = source("Pages.h")
        self.assertIn("page_requires_identity(path) && !remote_identity", pages)
        self.assertIn("Identification Required", pages)

    def test_setting_the_clock_stays_privileged(self):
        # Reading time status is public; only writing it is allow-listed, and
        # that lives on Transport's remote-management destination, not here.
        firmware = live_source("RNode_Firmware.ino")
        self.assertNotIn('register_request_handler("/time"', firmware)


if __name__ == "__main__":
    unittest.main()
