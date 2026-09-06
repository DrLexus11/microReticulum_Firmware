"""Knowing what a cadence costs, before choosing one.

TAKCapability.md §7 step 5. Accounting, not enforcement: these boards run under
approved laboratory conditions with both duty-cycle limits compiled to 0.0f,
and the constraint that will apply is on gain rather than on time. Nothing here
or on a node refuses to send anything, and these tests guard that as much as
they guard the arithmetic.
"""

import importlib.util
import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def load(name):
    path = os.path.join(ROOT, "tools", name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


budget = load("position_budget")


class ModelTests(unittest.TestCase):
    def test_it_reproduces_the_documented_cot_cost(self):
        # §2 measures raw CoT XML at 538 ms on air at the working point. If
        # this model cannot land near that, it is not modelling the same radio.
        ms = budget.time_on_air_ms(700 + 40, sf=7, bandwidth_hz=250000)
        self.assertAlmostEqual(ms, 538, delta=25)

    def test_a_compact_report_is_an_order_of_magnitude_cheaper(self):
        compact = budget.time_on_air_ms(60, sf=7, bandwidth_hz=250000)
        cot = budget.time_on_air_ms(740, sf=7, bandwidth_hz=250000)
        self.assertGreater(cot / compact, 8.0)

    def test_the_documented_fleet_fits_and_the_larger_one_does_not(self):
        # §2: ten nodes at one a minute fits, twenty-five does not.
        per_report = budget.time_on_air_ms(60, sf=7, bandwidth_hz=250000)
        for nodes, fits in ((10, True), (25, False)):
            occupancy = per_report * (3600.0 / 60.0 * nodes) / 3_600_000.0
            self.assertEqual(occupancy < 0.01, fits,
                             "%d nodes: %.3f%%" % (nodes, occupancy * 100))

    def test_a_slower_spreading_factor_costs_more(self):
        fast = budget.time_on_air_ms(60, sf=7, bandwidth_hz=250000)
        slow = budget.time_on_air_ms(60, sf=10, bandwidth_hz=250000)
        self.assertGreater(slow, fast * 4)

    def test_narrower_bandwidth_costs_more(self):
        wide = budget.time_on_air_ms(60, sf=7, bandwidth_hz=250000)
        narrow = budget.time_on_air_ms(60, sf=7, bandwidth_hz=125000)
        self.assertAlmostEqual(narrow / wide, 2.0, delta=0.1)

    def test_low_datarate_optimisation_engages_where_the_radio_needs_it(self):
        # At symbol times of 16 ms and above the receiver cannot track without
        # it, and the firmware switches it on at the same point.
        self.assertGreaterEqual(budget.symbol_time_ms(12, 125000), 16.0)
        self.assertLess(budget.symbol_time_ms(7, 250000), 16.0)


class AccountingIsNotEnforcementTests(unittest.TestCase):
    """The standing constraint: measure, never limit."""

    def test_the_node_counts_but_does_not_refuse(self):
        header = source("PositionReport.h")
        loop = header[header.index("inline void position_report_loop("):]
        # Every early return is about having nothing worth sending or nowhere
        # to send it -- never about having sent too much.
        for forbidden in ("airtime_limit", "duty", "throttle", "airtime_lock"):
            self.assertNotIn(forbidden, loop)

    def test_the_airtime_figure_comes_from_the_firmware_model(self):
        # A second copy of the time-on-air arithmetic would drift from the
        # first the day a modem is added, and the two would then disagree
        # about the budget while both looking right.
        header = source("PositionReport.h")
        self.assertIn("float packet_airtime_ms(uint16_t written);", header)
        self.assertIn("packet_airtime_ms((uint16_t)len)", header)

    def test_the_extraction_left_one_copy_of_the_maths(self):
        sketch = source("RNode_Firmware.ino")
        # add_airtime now calls the extracted function rather than repeating it.
        add = sketch[sketch.index("void add_airtime(uint16_t written) {"):]
        add = add[:add.index("\nvoid update_airtime()")]
        self.assertIn("packet_airtime_ms(written)", add)
        self.assertNotIn("lora_symbols", add)

    def test_the_spend_is_reported_where_it_can_be_read(self):
        pages = source("Pages.h")
        self.assertIn("Pos rpt", pages)
        self.assertIn("Pos air", pages)
        self.assertIn("position_report_duty_fraction", pages)

    def test_the_planner_states_a_verdict_without_imposing_one(self):
        tool = source("tools/position_budget.py")
        self.assertIn("Accounting, not enforcement", tool)
        # It prints guidance; it has no mechanism to stop anything.
        self.assertNotIn("sys.exit(1)", tool)


class CountersTests(unittest.TestCase):
    def test_the_node_tracks_what_it_spent(self):
        header = source("PositionReport.h")
        for field in ("bytes_sent", "airtime_ms"):
            self.assertIn(field, header)

    def test_the_duty_fraction_is_over_uptime_not_since_last_report(self):
        # A share of the channel means a share of wall-clock time. Dividing by
        # anything else produces a number that looks like a duty cycle and is
        # not one.
        header = source("PositionReport.h")
        fn = header[header.index("inline float position_report_duty_fraction()"):]
        fn = fn[:fn.index("\n}")]
        self.assertIn("node_uptime_seconds()", fn)
        self.assertIn("return 0.0f", fn)   # no division by a zero uptime


if __name__ == "__main__":
    unittest.main()
