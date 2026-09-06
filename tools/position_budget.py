#!/usr/bin/env python3
"""What a position-reporting cadence costs on air.

TAKCapability.md §7 step 5. Accounting, not enforcement: these boards run under
approved laboratory conditions with both duty-cycle limits compiled to 0.0f,
and the regulatory constraint that will apply is on gain rather than on time.
Nothing here or on the node refuses to send anything. The point is that a
cadence gets chosen on evidence rather than discovered in an exercise.

§2 gives one table for one working point. This computes it for whichever
working point a deployment actually uses:

    python tools/position_budget.py --nodes 10 --interval 60
    python tools/position_budget.py --sf 9 --bw 125000 --nodes 25 --interval 120

The time-on-air model is the LoRa one the firmware uses in
`packet_airtime_ms()`, which is the SX127x datasheet formula. Numbers from here
and numbers off a node should agree; if they stop agreeing, one of the two has
drifted and the node is the one to believe.
"""

import argparse

# Matches the firmware's PHY constants.
PHY_HEADER_LORA_SYMBOLS = 8
PHY_CRC_LORA_BITS = 16
DEFAULT_PREAMBLE_SYMBOLS = 8

# The compact position report at full extent, plus Reticulum's own framing.
# Twenty bytes of payload never travels alone.
POSITION_PAYLOAD_BYTES = 20
RETICULUM_OVERHEAD_BYTES = 40


def symbol_time_ms(sf, bandwidth_hz):
    return (2.0 ** sf) / bandwidth_hz * 1000.0


def time_on_air_ms(payload_bytes, sf, bandwidth_hz, coding_rate=5,
                   preamble_symbols=DEFAULT_PREAMBLE_SYMBOLS,
                   explicit_header=True):
    """Milliseconds on air for one packet.

    The same arithmetic as the firmware's packet_airtime_ms(), which is the
    datasheet formula. Low-datarate optimisation switches on where the firmware
    switches it on -- symbol times at or above 16 ms -- because at those
    settings the receiver cannot track without it.
    """
    t_sym = symbol_time_ms(sf, bandwidth_hz)
    low_datarate = 1 if t_sym >= 16.0 else 0

    numerator = (8 * payload_bytes + PHY_CRC_LORA_BITS - 4 * sf + 8 +
                 (PHY_HEADER_LORA_SYMBOLS if explicit_header else 0))
    denominator = 4 * (sf - 2 * low_datarate)
    symbols = numerator / denominator
    symbols *= coding_rate
    symbols += preamble_symbols + 0.25 + 8
    return symbols * t_sym


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sf", type=int, default=7, help="spreading factor")
    parser.add_argument("--bw", type=int, default=250000, help="bandwidth in Hz")
    parser.add_argument("--cr", type=int, default=5, help="coding rate denominator")
    parser.add_argument("--nodes", type=int, default=10)
    parser.add_argument("--interval", type=float, default=60.0,
                        help="seconds between reports from each node")
    parser.add_argument("--payload", type=int, default=POSITION_PAYLOAD_BYTES)
    parser.add_argument("--overhead", type=int, default=RETICULUM_OVERHEAD_BYTES,
                        help="Reticulum framing carried with the payload")
    parser.add_argument("--compare-cot", action="store_true",
                        help="show what raw CoT XML would have cost")
    args = parser.parse_args()

    on_air = args.payload + args.overhead
    per_packet_ms = time_on_air_ms(on_air, args.sf, args.bw, args.cr)
    reports_per_hour = 3600.0 / args.interval * args.nodes
    channel_ms_per_hour = per_packet_ms * reports_per_hour
    occupancy = channel_ms_per_hour / 3_600_000.0

    print("Working point : SF%d BW%d CR4/%d" % (args.sf, args.bw, args.cr))
    print("Symbol time   : %.3f ms" % symbol_time_ms(args.sf, args.bw))
    print("On air        : %d bytes (%d payload + %d framing)"
          % (on_air, args.payload, args.overhead))
    print("Per report    : %.1f ms" % per_packet_ms)
    print()
    print("Fleet         : %d nodes, one report every %.0f s"
          % (args.nodes, args.interval))
    print("Reports/hour  : %.0f" % reports_per_hour)
    print("Channel used  : %.2f%% of wall clock" % (occupancy * 100.0))
    print()

    # Not a limit, a reference. A shared half-duplex channel degrades well
    # before it is full, because two nodes transmitting at once lose both
    # packets and neither knows.
    if occupancy < 0.01:
        verdict = "comfortable"
    elif occupancy < 0.05:
        verdict = "workable, but collisions start to matter"
    elif occupancy < 0.10:
        verdict = "crowded -- expect losses on a half-duplex channel"
    else:
        verdict = "over budget for anything else to share the channel"
    print("Verdict       : %s" % verdict)
    print()
    print("How many nodes fit at this cadence, per share of channel:")
    for share in (0.01, 0.02, 0.05):
        capacity = share * 3_600_000.0 / (per_packet_ms * 3600.0 / args.interval)
        print("  %4.0f%%  %5.1f nodes" % (share * 100.0, capacity))

    if args.compare_cot:
        cot_ms = time_on_air_ms(700 + args.overhead, args.sf, args.bw, args.cr)
        print()
        print("Raw CoT XML   : %.1f ms per report (%.1fx this format)"
              % (cot_ms, cot_ms / per_packet_ms))
        print("               %.0f reports/hour fills the channel"
              % (3_600_000.0 / cot_ms))


if __name__ == "__main__":
    main()
