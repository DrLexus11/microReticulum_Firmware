"""Architecture checks for pulling UTC from a peer.

A node only ever got time because a human ran a CLI at it, which does not
survive a deployment of apartments. These guard the properties that make the
automatic path safe rather than merely working.
"""

import os
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


class TimeSyncTests(unittest.TestCase):
    def test_callbacks_never_tear_down_their_own_link(self):
        # Destroying the link from inside its own response callback panics the
        # core -- Guru Meditation, unhandled debug exception, observed
        # immediately after a successful adoption. Callbacks record an outcome;
        # the loop releases the link where that is safe.
        sync = source("TimeSync.h")
        finish = sync[sync.index("inline void time_sync_finish("):
                      sync.index("inline void time_sync_release(")]
        self.assertNotIn("teardown", finish)
        self.assertIn("st.finished = true", finish)
        release = sync[sync.index("inline void time_sync_release("):
                       sync.index("inline void time_sync_response(")]
        self.assertIn("teardown", release)
        loop = sync[sync.index("inline void time_sync_loop("):]
        self.assertIn("if (st.finished) { time_sync_release(); return; }", loop)

    def test_adoption_requires_a_strictly_better_stratum(self):
        # Two nodes at the same stratum would each keep adopting from the other.
        sync = source("TimeSync.h")
        self.assertIn("peer_stratum >= mine", sync)
        self.assertIn("peer_stratum + 1", sync)

    def test_a_restored_clock_always_seeks_a_source(self):
        sync = source("TimeSync.h")
        wants = sync[sync.index("inline bool time_sync_wants_time("):
                     sync.index("inline void time_sync_loop(")]
        self.assertIn("WallTimeSource::PERSISTED) return true", wants)
        self.assertIn("!OS::wall_time_known()) return true", wants)
        # A stamp from a previous boot can sit ahead of the monotonic clock.
        self.assertIn("if (verified > now) return true", wants)

    def test_the_peer_is_configurable_not_compiled_in(self):
        provisioning = source("Provisioning.cpp")
        self.assertIn('field_bytes("Time Peer", PROV_GENERAL_TIME_PEER', provisioning)
        self.assertIn("#define PROV_GENERAL_TIME_PEER", source("Provisioning.h"))
        # Empty hash disables it, so an unconfigured node does nothing.
        self.assertIn("time_sync_peer_hash.size() == 0) return", source("TimeSync.h"))


if __name__ == "__main__":
    unittest.main()
