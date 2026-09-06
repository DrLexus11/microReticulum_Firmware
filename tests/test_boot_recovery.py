"""A node must not be able to brick itself with its own caches.

`Reticulum::start()` loads the path, known-destination and packet-hash stores
into RAM before it does anything else, and nothing bounds the read: the
on-flash index is replayed in full and only then pruned to the configured
maximum. A board that has been up for days accumulates enough that the load no
longer fits. The allocation that fails is inside a container, so it throws
`std::bad_alloc` with nothing to catch it -- the node aborts, reboots, and
reads the same store again.

That loop is unrecoverable without a console, and it selects for exactly the
nodes that have been running longest. These check the escape hatch, because
every one of them fails silently: a wipe that runs too late, a streak that
clears itself, or a store the wipe does not know about all look like working
code right up until a board is dead on a hillside.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBDEPS = os.path.join(ROOT, ".pio", "libdeps", "ozdisan-esp32-espnow")


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def define(header, name):
    match = re.search(r"^#define\s+%s\s+(.+)$" % re.escape(name), header,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    return match.group(1).strip()


class WipeOrderTests(unittest.TestCase):
    def test_the_wipe_runs_before_the_stores_are_read(self):
        # After start() there is nothing left to save: the allocation has
        # already failed and the node is in the panic handler.
        sketch = source("RNode_Firmware.ino")
        wipe = sketch.index("node_clear_persisted_caches()")
        start = sketch.index("reticulum.start()")
        self.assertLess(wipe, start)

    def test_the_wipe_is_gated_on_a_streak_not_a_single_fault(self):
        # One panic is a bug. Three boots that never reached the loop is a
        # board that cannot start with the store it has -- and paths are worth
        # keeping until then, because relearning them costs airtime.
        sketch = source("RNode_Firmware.ino")
        self.assertIn("if (node_caches_are_suspect())", sketch)
        header = source("NodeStatus.h")
        self.assertGreaterEqual(int(define(header, "NODE_CACHE_WIPE_FAULTS")), 2)


class StreakTests(unittest.TestCase):
    def test_a_wipe_does_not_declare_the_board_healthy(self):
        # If the caches were not the cause, clearing the streak here would hide
        # that: the next boot starts from zero and the board loops forever with
        # no evidence it ever tried.
        body = source("NodeStatus.cpp")
        wipe = body[body.index("void node_clear_persisted_caches()"):
                    body.index("void node_boot_mark_healthy()")]
        self.assertNotIn("node_fault_streak = 0", wipe)

    def test_only_a_surviving_boot_clears_the_streak(self):
        body = source("NodeStatus.cpp")
        healthy = body[body.index("void node_boot_mark_healthy()"):]
        self.assertIn("node_uptime_seconds() < NODE_BOOT_HEALTHY_S", healthy)
        self.assertIn("node_fault_streak = 0", healthy)
        # The uptime check has to come first, or the first pass through the
        # loop clears the streak on a board that is about to die again.
        self.assertLess(healthy.index("NODE_BOOT_HEALTHY_S"),
                        healthy.index("node_fault_streak = 0"))

    def test_the_healthy_threshold_is_past_the_store_load(self):
        # The load happens inside start(), seconds into the boot. A threshold
        # anywhere near it would clear the streak on a board that panics on its
        # first announce instead.
        header = source("NodeStatus.h")
        self.assertGreaterEqual(int(define(header, "NODE_BOOT_HEALTHY_S")), 60)

    def test_every_abnormal_restart_counts_toward_the_streak(self):
        # A store too big to load can take the node out through the watchdog
        # just as easily as through an abort -- the allocation stalls rather
        # than fails. Counting only panics would leave that board looping.
        body = source("NodeStatus.cpp")
        record = body[body.index("void node_restart_counts_record_boot()"):
                      body.index("bool node_caches_are_suspect()")]
        self.assertEqual(record.count("node_fault_streak++"), 2)
        self.assertIn("ESP_RST_PANIC", record)
        self.assertIn("ESP_RST_TASK_WDT", record)

    def test_a_power_cycle_does_not_clear_the_streak(self):
        # The operator pulling the plug and trying again does not make the
        # store any less poisoned, and it is the first thing anyone does.
        body = source("NodeStatus.cpp")
        record = body[body.index("void node_restart_counts_record_boot()"):
                      body.index("bool node_caches_are_suspect()")]
        default = record[record.index("default:"):]
        self.assertNotIn("node_fault_streak", default)

    def test_the_streak_survives_a_reflash_of_an_older_firmware(self):
        # The counts file grew from two fields to three. A strict length check
        # would silently drop a board's lifetime crash and panic counts.
        body = source("NodeStatus.cpp")
        load = body[body.index("void node_restart_counts_load()"):
                    body.index("static void node_restart_counts_store()")]
        self.assertIn("read >= sizeof(uint32_t) * 2", load)
        self.assertIn("read >= sizeof(values)", load)


class CoverageTests(unittest.TestCase):
    def test_the_wipe_covers_every_store_transport_loads(self):
        # A store added to the library that the wipe does not know about is a
        # boot loop that comes back after the wipe reports success.
        transport = os.path.join(LIBDEPS, "microReticulum", "src",
                                 "microReticulum", "Transport.cpp")
        if not os.path.exists(transport):
            self.skipTest("library sources are not fetched")
        with open(transport, encoding="utf-8") as handle:
            text = handle.read()
        loaded = set(re.findall(r'"\./(\w+_store)/"', text))
        self.assertTrue(loaded, "no persisted stores found in Transport.cpp")
        wiped = set(re.findall(r'node_remove_store\("\./(\w+)"\)',
                               source("NodeStatus.cpp")))
        self.assertEqual(loaded - wiped, set())

    def test_the_wipe_joins_the_basenames_the_callback_hands_back(self):
        # listDirectory yields bare names. Passing one straight to remove()
        # deletes nothing and reports success, which is the worst outcome
        # available here.
        body = source("NodeStatus.cpp")
        walk = body[body.index("static uint16_t node_remove_store("):
                    body.index("void node_clear_persisted_caches()")]
        self.assertIn('snprintf(full, sizeof(full), "%s/%s", path, name)', walk)
        self.assertIn("filesystem.remove(full)", walk)

    def test_the_wipe_announces_itself_before_it_runs(self):
        # This runs on a board nobody can log into. If the wipe is what kills
        # it, the last line out of the port has to be the one that says so.
        body = source("NodeStatus.cpp")
        wipe = body[body.index("void node_clear_persisted_caches()"):
                    body.index("void node_boot_mark_healthy()")]
        self.assertLess(wipe.index("printf("), wipe.index("node_remove_store"))


class ConsoleTests(unittest.TestCase):
    """The serial console is the only diagnostic channel a fielded node has."""

    def test_the_wifi_status_poll_does_not_print_every_poll(self):
        # WIFI_UPDATE_INTERVAL_MS is 500, so an unconditional print is two
        # lines a second forever on any board whose station never comes up --
        # which is every ESP-NOW-only node by design. It buries the boot
        # banner, the heap samples and the panic backtrace alike.
        remote = source("Remote.h")
        update = remote[remote.index("void wifi_update_status()"):]
        update = update[:update.index("\n}")]
        self.assertIn("wr_wifi_status != previous", update)
        self.assertIn("WIFI_STATUS_REPORT_MS", update)

    def test_an_unchanged_status_still_repeats_eventually(self):
        # A station stuck at WL_DISCONNECTED for an hour is worth knowing
        # about. Silence after the first line would hide it.
        remote = source("Remote.h")
        interval = int(define(remote, "WIFI_STATUS_REPORT_MS").rstrip("ULul"))
        poll = int(define(remote, "WIFI_UPDATE_INTERVAL_MS"))
        self.assertGreater(interval, poll * 10)

    def test_the_first_status_is_always_reported(self):
        # wr_wifi_status starts at WL_IDLE_STATUS, which a board can genuinely
        # be in -- comparing against it alone would swallow the first report.
        remote = source("Remote.h")
        update = remote[remote.index("void wifi_update_status()"):]
        update = update[:update.index("\n}")]
        self.assertIn("!reported", update)


if __name__ == "__main__":
    unittest.main()
