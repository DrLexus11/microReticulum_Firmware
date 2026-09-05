"""Duty-cycle enforcement must not silence what matters.

Blocking the whole transmit queue at the limit treats a distress message
exactly like a position beacon -- and beaconing is the workload that exhausts
the budget, so the traffic that caused the shortage would silence the traffic
that matters. These pin the properties that stop that happening.
"""

import os
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


class AirtimePriorityTests(unittest.TestCase):
    def setUp(self):
        self.firmware = source("RNode_Firmware.ino")

    def test_queued_packets_carry_a_class(self):
        # A parallel FIFO, pushed at both enqueue sites, or the classes drift
        # out of step with the packets they describe.
        self.assertIn("FIFOBuffer16 packet_classes", self.firmware)
        self.assertIn("fifo16_init(&packet_classes", self.firmware)
        self.assertEqual(self.firmware.count("fifo16_push(&packet_classes"), 2)
        self.assertEqual(self.firmware.count("fifo16_pop(&packet_classes"), 2)

    def test_only_announces_are_classed_routine(self):
        # The modem cannot read an encrypted payload, but the Reticulum header's
        # low two bits give the packet type in the clear.
        fn = self.firmware[self.firmware.index("static inline uint16_t tx_class_for_rns"):]
        fn = fn[:fn.index("\n}") + 2]
        self.assertIn("0b00000011", fn)
        self.assertIn("TX_CLASS_ROUTINE", fn)
        self.assertIn("TX_CLASS_NORMAL", fn)

    def test_host_traffic_is_never_classed_routine(self):
        # We cannot see inside it, and guessing wrong drops something deliberate.
        serial = self.firmware[self.firmware.index("void serial_callback("):]
        serial = serial[:serial.index("fifo16_push(&packet_classes") + 200]
        self.assertIn("fifo16_push(&packet_classes, TX_CLASS_NORMAL)", serial)

    def test_pressure_engages_before_the_hard_limit(self):
        # Reaching airtime_lock stops everything. Pressure exists to make that
        # far less likely by spending the last of the budget on traffic that
        # cannot simply be sent again.
        self.assertIn("AIRTIME_PRESSURE_FRACTION 0.75f", self.firmware)
        self.assertIn("longterm_airtime >= lt_airtime_limit * AIRTIME_PRESSURE_FRACTION",
                      self.firmware)
        # And it must remain inert while no limit is configured.
        self.assertIn("lt_airtime_limit != 0.0 &&", self.firmware)

    def test_both_drain_paths_honour_pressure(self):
        for fn_name in ("void flush_queue", "void pop_queue"):
            fn = self.firmware[self.firmware.index(fn_name):]
            fn = fn[:fn.index("\n}\n")]
            self.assertIn("TX_CLASS_ROUTINE", fn, fn_name)
            self.assertIn("airtime_pressure", fn, fn_name)
            self.assertIn("tx_deferred_routine++", fn, fn_name)

    def test_the_effect_is_observable(self):
        self.assertIn("pressure=%d routine_dropped=%lu", self.firmware)


if __name__ == "__main__":
    unittest.main()
