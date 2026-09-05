"""Architecture checks for pulling UTC from a peer.

A node only ever got time because a human ran a CLI at it, which does not
survive a deployment of apartments. These guard the properties that make the
automatic path safe rather than merely working.
"""

import os
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip_comments(text):
    """Drop // comments so an assertion about the code cannot match prose."""
    return "\n".join(line.split("//")[0] for line in text.splitlines())


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

    def test_a_nonce_is_required_for_a_signed_assertion(self):
        # A signed timestamp does not prove *current* time. Without a nonce
        # bound into the signature, a replayed assertion is indistinguishable
        # from a fresh one to a node that has no clock -- which is exactly the
        # node that needs the answer.
        sync = source("TimeSync.h")
        self.assertIn("echoed_nonce != st.nonce", sync)
        # Drawn from the library's CSPRNG, and all eight bytes of it. The
        # previous assertion pinned the literal esp_random() call, which made
        # this test a lock on one MCU's API rather than on the property that
        # matters -- and the header compiles under HAS_RNS on nRF52 too, where
        # esp_random does not exist.
        self.assertIn("RNS::Cryptography::random(8)", sync)
        # Comments stripped: the code must not call these, but the note above
        # the call names them to explain why not.
        code = strip_comments(sync)
        self.assertNotIn("esp_random", code)
        # Not randomnum(): it builds its 32 bits out of byte zero four times
        # over, so it yields eight bits of entropy, and a guessable nonce is a
        # replayable one.
        self.assertNotIn("randomnum", code)
        self.assertIn("if (st.nonce == 0) st.nonce = 1", sync)

    def test_verification_uses_the_identity_the_link_was_opened_against(self):
        # "Time Peer" is live-applied, so re-recalling the identity from the
        # global at verification time can check the signature against a
        # different peer than the one that answered, and reject a valid
        # response as a forgery.
        sync = source("TimeSync.h")
        self.assertIn("st.peer_identity = peer_identity", sync)
        self.assertIn("st.peer_identity.validate(signature", sync)
        response = sync[sync.index("inline void time_sync_response("):
                        sync.index("inline void time_sync_link_established(")]
        self.assertNotIn("Identity::recall(time_sync_peer_hash)", response)

    def test_authorities_are_opt_in_but_binding_once_set(self):
        # Empty means IFAC-membership trust, which is the v1 behaviour, so
        # enabling this cannot silently break a working node. Non-empty means a
        # valid signature from a listed identity is required.
        sync = source("TimeSync.h")
        self.assertIn("if (!time_sync_authorities.empty())", sync)
        self.assertIn("peer is not a time authority", sync)
        self.assertIn("time assertion failed signature check", sync)
        self.assertIn("st.peer_identity.validate(signature", sync)

    def test_the_peer_is_configurable_not_compiled_in(self):
        provisioning = source("Provisioning.cpp")
        self.assertIn('field_bytes("Time Peer", PROV_GENERAL_TIME_PEER', provisioning)
        self.assertIn("#define PROV_GENERAL_TIME_PEER", source("Provisioning.h"))
        # Empty hash disables it, so an unconfigured node does nothing.
        self.assertIn("time_sync_peer_hash.size() == 0) return", source("TimeSync.h"))


if __name__ == "__main__":
    unittest.main()
