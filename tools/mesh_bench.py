#!/usr/bin/env python3
"""Repeatable reliability and latency measurement for mesh nodes.

Why this exists
---------------
On 2026-09-03 an afternoon of A/B testing produced conclusions that could not
be defended. Rev 1's own link setup drifted from 0.37 s median to 0.96 s over a
few hours with no code change on that path, and a node that answered first try
failed 4/4 minutes later. Measuring a baseline, changing something, and
measuring again gives you the change plus however much the 2.4 GHz environment
moved underneath you -- and here the second term was larger than the first.

Two rules follow, and they are the whole point of this tool:

1. **Interleave, never sequence.** Targets are visited round-robin inside one
   time window, so drift hits every target roughly equally. Comparing two
   targets in the same run is sound; comparing two runs an hour apart is not.

2. **Always carry a control.** A target reachable over a path you are not
   changing (Rev 1 over UDP) is measured alongside the ones you are. If the
   control moved between runs, the environment moved, and any difference in the
   others means nothing until you re-run.

Results are appended as JSON lines so runs can be compared later, and so a
claim can be checked against the samples that produced it rather than a
remembered summary.

Usage
-----
  mesh_bench.py --target rev1=9b0cc8ac... --target ozd02=558a7fc0... \\
                --control rev1 --rounds 20 --label before-fragment-retry

  mesh_bench.py --compare runs/before-fragment-retry.jsonl runs/after.jsonl
"""

import argparse
import json
import os
import statistics
import sys
import time

import RNS


DEFAULT_STORE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "..", "bench-runs")


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


class Trial:
    """One link setup plus one page request against a single node."""

    def __init__(self, name, dest_hash, path, identity, timeout):
        self.name = name
        self.dest_hash = dest_hash
        self.path = path
        self.identity = identity
        self.timeout = timeout

    def run(self):
        started = time.time()
        result = {
            "target": self.name,
            "started": started,
            "ok": False,
            "stage": "path",
            "link_rtt": None,
            "setup_s": None,
            "total_s": None,
            "bytes": 0,
        }

        if not RNS.Transport.has_path(self.dest_hash):
            RNS.Transport.request_path(self.dest_hash)
            deadline = time.time() + min(30.0, self.timeout)
            while (not RNS.Transport.has_path(self.dest_hash)
                   and time.time() < deadline):
                time.sleep(0.1)
        if not RNS.Transport.has_path(self.dest_hash):
            result["total_s"] = time.time() - started
            return result

        result["hops"] = RNS.Transport.hops_to(self.dest_hash)
        identity = RNS.Identity.recall(self.dest_hash)
        if identity is None:
            result["stage"] = "identity"
            result["total_s"] = time.time() - started
            return result

        destination = RNS.Destination(identity, RNS.Destination.OUT,
                                      RNS.Destination.SINGLE,
                                      "nomadnetwork", "node")
        state = {"done": False}
        result["stage"] = "link"
        link = RNS.Link(destination)

        def on_response(receipt):
            body = receipt.response
            result["bytes"] = len(body) if body else 0
            result["ok"] = result["bytes"] > 0
            state["done"] = True

        def on_failed(_receipt):
            result["stage"] = "request-refused"
            state["done"] = True

        def established(lk):
            result["link_rtt"] = lk.rtt
            result["setup_s"] = time.time() - started
            result["stage"] = "request"
            lk.identify(self.identity)
            lk.request(self.path, data=None, response_callback=on_response,
                       failed_callback=on_failed)

        link.set_link_established_callback(established)
        deadline = time.time() + self.timeout
        while not state["done"] and time.time() < deadline:
            time.sleep(0.05)
        result["total_s"] = time.time() - started
        try:
            link.teardown()
        except Exception:
            pass
        return result


def preflight(trials, attempts=3):
    """Prove every target actually serves the probe page before measuring.

    The first run of this tool reported Rev 2 at 0% success and Rev 1 at 100%,
    which read as a dead board. Rev 2 was fine: it runs an older image with no
    /page/time.mu, so every request timed out unanswered. A harness that can
    report a healthy node as failing is worse than no harness, because the
    number looks like data.

    A link that establishes and then produces no response, repeatedly, means the
    path is not served. A link that will not establish is flakiness, which is
    the thing we are here to measure -- so those are treated differently.
    """
    unusable = []
    for trial in trials:
        stages = []
        for _ in range(attempts):
            sample = trial.run()
            if sample["ok"]:
                stages = []
                break
            stages.append(sample["stage"])
            time.sleep(1.0)
        if stages and all(stage.startswith("request") for stage in stages):
            unusable.append(trial.name)
            print("  %-10s links fine but never answers %s -- not served here"
                  % (trial.name, trial.path))
        elif stages:
            print("  %-10s did not answer in %d attempts (%s); measuring anyway,"
                  " this may be the flakiness we are looking for"
                  % (trial.name, attempts, ", ".join(stages)))
        else:
            print("  %-10s ok" % trial.name)
    return unusable


def summarise(samples, name):
    mine = [s for s in samples if s["target"] == name]
    ok = [s for s in mine if s["ok"]]
    setups = [s["setup_s"] for s in ok if s["setup_s"] is not None]
    totals = [s["total_s"] for s in ok]
    stages = {}
    for sample in mine:
        if not sample["ok"]:
            stages[sample["stage"]] = stages.get(sample["stage"], 0) + 1
    return {
        "target": name,
        "trials": len(mine),
        "ok": len(ok),
        "success_pct": (100.0 * len(ok) / len(mine)) if mine else 0.0,
        "setup_median": statistics.median(setups) if setups else None,
        "setup_p90": percentile(setups, 0.90),
        "total_median": statistics.median(totals) if totals else None,
        "failed_at": stages,
    }


def render(summaries, control):
    width = max(len(s["target"]) for s in summaries)
    print("\n%-*s  %7s  %8s  %12s  %10s" % (
        width, "target", "trials", "success", "setup median", "setup p90"))
    for summary in summaries:
        marker = "  (control)" if summary["target"] == control else ""
        print("%-*s  %7d  %6.1f%%  %11s  %9s%s" % (
            width, summary["target"], summary["trials"], summary["success_pct"],
            "%.3f s" % summary["setup_median"] if summary["setup_median"] else "-",
            "%.3f s" % summary["setup_p90"] if summary["setup_p90"] else "-",
            marker))
        if summary["failed_at"]:
            detail = ", ".join("%s=%d" % kv for kv in
                               sorted(summary["failed_at"].items()))
            print("%-*s  failures by stage: %s" % (width, "", detail))


def compare(before_path, after_path, control):
    def load(path):
        with open(path, encoding="utf-8") as handle:
            return [json.loads(line) for line in handle if line.strip()]

    before, after = load(before_path), load(after_path)
    names = sorted({s["target"] for s in before} | {s["target"] for s in after})

    # Reliability and latency drift independently, and conflating them hides
    # real results. Observed 2026-09-04: the control held at exactly 11/12 in
    # both runs while its median setup moved 1.56x, and a target went 33% ->
    # 58% success. Judging that on the latency ratio alone would have thrown
    # away a genuine 25-point reliability gain.
    def control_axes():
        if not control:
            return None, None
        b, a = summarise(before, control), summarise(after, control)
        rate = None
        if b["trials"] and a["trials"]:
            rate = a["success_pct"] - b["success_pct"]
        latency = None
        if b["setup_median"] and a["setup_median"]:
            latency = a["setup_median"] / b["setup_median"]
        return rate, latency

    control_rate_shift, control_latency_shift = control_axes()

    print("\n%-16s  %18s  %18s" % ("target", "before", "after"))
    for name in names:
        b, a = summarise(before, name), summarise(after, name)
        print("%-16s  %5.1f%% %10s  %5.1f%% %10s" % (
            name, b["success_pct"],
            "%.3f s" % b["setup_median"] if b["setup_median"] else "-",
            a["success_pct"],
            "%.3f s" % a["setup_median"] if a["setup_median"] else "-"))

    if control_rate_shift is None:
        print("\nNo control in both runs. This comparison cannot separate your "
              "change from the environment -- treat it as suggestive only.")
        return

    print("\nControl moved: success %+.1f points, median setup %.2fx." % (
        control_rate_shift, control_latency_shift or 1.0))
    if abs(control_rate_shift) <= 5.0:
        print("  RELIABILITY comparison is sound -- the control's success rate "
              "held, so success-rate differences larger than a few points are "
              "the change, not the room.")
    else:
        print("  RELIABILITY comparison is NOT sound -- the control's own "
              "success rate moved %+.1f points. Re-run both arms interleaved."
              % control_rate_shift)
    if control_latency_shift and 0.8 <= control_latency_shift <= 1.25:
        print("  LATENCY comparison is sound.")
    else:
        print("  LATENCY comparison is NOT sound -- the control's median setup "
              "moved %.2fx on its own. Ignore latency differences below that."
              % (control_latency_shift or 1.0))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", action="append", default=[],
                        metavar="NAME=HASH",
                        help="node to measure; repeat for each")
    parser.add_argument("--control", metavar="NAME",
                        help="target whose path you are NOT changing")
    parser.add_argument("--rounds", type=int, default=20,
                        help="visits per target (default 20)")
    parser.add_argument("--page", default="/page/time.mu",
                        help="page to request each visit")
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--settle", type=float, default=2.0,
                        help="seconds between trials")
    parser.add_argument("--label", help="name for this run's sample file")
    parser.add_argument("--store", default=DEFAULT_STORE)
    parser.add_argument("--identity",
                        default="~/.nomadnetwork/storage/identity")
    parser.add_argument("--force", action="store_true",
                        help="measure even targets that failed preflight")
    parser.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"),
                        help="compare two saved runs instead of measuring")
    args = parser.parse_args()

    if args.compare:
        compare(args.compare[0], args.compare[1], args.control)
        return 0

    if not args.target:
        sys.exit("no targets given\n" + parser.format_usage())

    targets = []
    for entry in args.target:
        if "=" not in entry:
            sys.exit("--target wants NAME=HASH, got %r" % entry)
        name, hexhash = entry.split("=", 1)
        try:
            raw = bytes.fromhex(hexhash)
        except ValueError:
            sys.exit("not a hex destination hash for %s: %r" % (name, hexhash))
        if len(raw) != RNS.Reticulum.TRUNCATED_HASHLENGTH // 8:
            sys.exit("%s: hash is %d bytes, expected %d" % (
                name, len(raw), RNS.Reticulum.TRUNCATED_HASHLENGTH // 8))
        targets.append((name, raw))

    names = [n for n, _ in targets]
    if args.control and args.control not in names:
        sys.exit("control %r is not one of the targets" % args.control)
    if not args.control:
        print("WARNING: no --control. Drift will be invisible and this run "
              "cannot be compared against another with any confidence.")

    RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"),
                  loglevel=int(os.environ.get("RNSLL", "2")))
    identity = RNS.Identity.from_file(os.path.expanduser(args.identity))

    trials = [Trial(name, raw, args.page, identity, args.timeout)
              for name, raw in targets]

    print("preflight: checking every target serves %s" % args.page)
    unusable = preflight(trials)
    if unusable and not args.force:
        sys.exit("\nthese targets do not serve %s: %s\n"
                 "Pick a page they all have (/page/index.mu is served by every "
                 "build) or pass --force to measure them anyway."
                 % (args.page, ", ".join(unusable)))

    os.makedirs(args.store, exist_ok=True)
    label = args.label or time.strftime("run-%Y%m%d-%H%M%S")
    out_path = os.path.join(args.store, label + ".jsonl")

    samples = []
    started = time.time()
    print("measuring %d targets x %d rounds, interleaved -> %s"
          % (len(trials), args.rounds, out_path))
    with open(out_path, "a", encoding="utf-8") as handle:
        for round_index in range(args.rounds):
            # Round-robin, so environmental drift lands on every target
            # roughly equally rather than on whichever went last.
            for trial in trials:
                sample = trial.run()
                sample["round"] = round_index
                samples.append(sample)
                handle.write(json.dumps(sample) + "\n")
                handle.flush()
                print("  r%-3d %-10s %s %s" % (
                    round_index, trial.name,
                    "ok " if sample["ok"] else "FAIL",
                    ("%.3f s" % sample["setup_s"]) if sample["setup_s"]
                    else "(" + sample["stage"] + ")"))
                time.sleep(args.settle)

    elapsed = time.time() - started
    print("\n%d trials over %.1f minutes" % (len(samples), elapsed / 60.0))
    render([summarise(samples, name) for name in names], args.control)
    print("\nsamples: %s" % out_path)
    if elapsed < 300:
        print("NOTE: this run is under five minutes. The bench has been seen "
              "to drift by more than 2x within an hour, so short runs are "
              "weak evidence even when interleaved.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
