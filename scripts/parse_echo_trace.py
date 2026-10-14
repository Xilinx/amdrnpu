#!/usr/bin/env python3
"""Parse tessai-agent echo-test perf traces and report per-step latency stats.

Input is `perf script` style output, one line per trace event:

    tessai_agent_ec    1241 [007]     0.000000:    sdt_tessai_agent:cmd_submit_enter: (ffff...) arg1=... arg2=0
    ...

A "run" starts at every `sdt_tessai_agent:cmd_submit_enter` (step 0) and
extends to the next one. Within a run, the events listed in EXCLUDED_EVENTS
are dropped, and the latency of a step is the time from that step's event to
the next *kept* event. Because the excluded host-side book-keeping events sit
between `tessai_rpmsg_send_exit` and `zynqmp_ipi_gic_entry`, dropping them makes
the `tessai_rpmsg_send_exit` step span the full RPU round-trip.

Outputs min/max/avg per step and a text histogram, and writes a Markdown report.
"""

import argparse
import math
import re
import sys
from collections import OrderedDict

# Events whose step latency is not reported. A step's latency is always the
# gap to the *immediately following* raw event; excluding an event only drops
# that event's own outgoing step from the report. The gaps produced by the
# excluded events (e.g. the host wait spanning wait_ioctl_enter -> gic_entry)
# are therefore not attributed to any reported step, but they are still counted
# in the end-to-end total.
EXCLUDED_EVENTS = {
    "tessai_agent:tessai_cmd_submit_exit",
    "sdt_tessai_agent:cmd_submit_exit",
    "sdt_tessai_agent:wait_ioctl_enter",
}

# A run begins at each occurrence of this event.
RUN_START_EVENT = "sdt_tessai_agent:cmd_submit_enter"

# Extra steps measured directly between two events, spanning any excluded
# events in between. Used to recover a meaningful latency (e.g. the RPU
# round-trip wait) that would otherwise be dropped by the exclusions above.
BRIDGE_STEPS = [
    ("tessai_agent:tessai_rpmsg_send_exit", "zynqmp_ipi_gic:zynqmp_ipi_gic_entry"),
]

LINE_RE = re.compile(
    r"^\s*(?P<comm>.+?)\s+(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+"
    r"(?P<ts>\d+\.\d+):\s+(?P<event>[\w.]+:[\w.]+):\s*(?P<args>.*)$"
)


def event_key(event, args):
    """Disambiguate events that repeat within a run (e.g. ipi_send TX vs RX)."""
    if event == "zynqmp_ipi:zynqmp_ipi_send":
        m = re.search(r"chan=(TX|RX)", args)
        if m:
            return f"{event}({m.group(1)})"
    return event


def parse_events(lines):
    """Yield (event_key, timestamp_seconds) for every recognized trace line."""
    for line in lines:
        m = LINE_RE.match(line)
        if not m:
            continue
        key = event_key(m.group("event"), m.group("args"))
        yield key, float(m.group("ts"))


def split_runs(events):
    """Split the flat event stream into runs starting at RUN_START_EVENT."""
    runs = []
    current = None
    for key, ts in events:
        if key == RUN_START_EVENT:
            if current:
                runs.append(current)
            current = []
        if current is not None:
            current.append((key, ts))
    if current:
        runs.append(current)
    return runs


def run_step_deltas(run):
    """Return ordered [(step_label, delta_us)] for one run.

    Each step's latency is the gap to the immediately following raw event.
    Steps that touch an excluded event on either end are omitted, and any
    BRIDGE_STEPS are added, measured directly across the excluded events.
    """
    scored = []  # (sort_key, label, delta_us)
    for i in range(len(run) - 1):
        (from_key, from_ts) = run[i]
        (to_key, to_ts) = run[i + 1]
        if from_key in EXCLUDED_EVENTS or to_key in EXCLUDED_EVENTS:
            continue
        scored.append((float(i), f"{from_key} -> {to_key}",
                       (to_ts - from_ts) * 1e6))

    for a, b in BRIDGE_STEPS:
        ai = next((i for i, (k, _) in enumerate(run) if k == a), None)
        if ai is None:
            continue
        bi = next((i for i, (k, _) in enumerate(run) if k == b and i > ai), None)
        if bi is None:
            continue
        # ai + 0.5 slots the bridge right after the source event's position.
        scored.append((ai + 0.5, f"{a} -> {b}",
                       (run[bi][1] - run[ai][1]) * 1e6))

    scored.sort(key=lambda x: x[0])
    return [(label, d) for _, label, d in scored]


def run_total_us(run):
    """Full end-to-end span of a run (includes excluded gaps like host wait)."""
    return (run[-1][1] - run[0][1]) * 1e6


def aggregate(runs):
    """Aggregate step deltas across runs, preserving first-seen step order.

    Returns (steps, totals) where steps is an OrderedDict step_index -> dict.
    """
    samples = OrderedDict()  # step_index -> {"label": str, "values": [..]}
    totals = []
    for run in runs:
        if len(run) >= 2:
            totals.append(run_total_us(run))
        for idx, (label, dus) in enumerate(run_step_deltas(run)):
            if idx not in samples:
                samples[idx] = {"label": label, "values": []}
            # Guard against inconsistent ordering between runs.
            if samples[idx]["label"] != label:
                samples[idx]["label"] += f" | {label}"
            samples[idx]["values"].append(dus)
    return samples, totals


def stats(values):
    n = len(values)
    mn = min(values)
    mx = max(values)
    avg = sum(values) / n
    if n > 1:
        var = sum((v - avg) ** 2 for v in values) / (n - 1)
        std = math.sqrt(var)
    else:
        std = 0.0
    return n, mn, mx, avg, std


def histogram(values, bins=10, width=40):
    """Return list of (lo, hi, count, bar) text histogram rows."""
    mn, mx = min(values), max(values)
    if mx == mn:
        return [(mn, mx, len(values), "#" * width)]
    step = (mx - mn) / bins
    counts = [0] * bins
    for v in values:
        b = int((v - mn) / step)
        if b == bins:
            b -= 1
        counts[b] += 1
    peak = max(counts) or 1
    rows = []
    for i, c in enumerate(counts):
        lo = mn + i * step
        hi = lo + step
        bar = "#" * int(round(width * c / peak))
        rows.append((lo, hi, c, bar))
    return rows


def build_report(samples, totals, bins):
    lines = []
    a = lines.append
    a("# tessai-agent echo test latency statistics\n")
    a(f"Runs analyzed: **{len(totals) if totals else 'n/a'}**  ")
    a(f"Excluded steps: {', '.join(sorted(EXCLUDED_EVENTS))}\n")

    a("## Per-step summary (microseconds)\n")
    a("| Step | Event | Count | Min | Max | Avg | Std |")
    a("|-----:|-------|------:|----:|----:|----:|----:|")
    for idx, info in samples.items():
        n, mn, mx, avg, std = stats(info["values"])
        a(f"| {idx} | `{info['label']}` | {n} | "
          f"{mn:.3f} | {mx:.3f} | {avg:.3f} | {std:.3f} |")
    a("")

    if totals:
        n, mn, mx, avg, std = stats(totals)
        a("## End-to-end total (microseconds, full span incl. excluded gaps)\n")
        a("| Count | Min | Max | Avg | Std |")
        a("|------:|----:|----:|----:|----:|")
        a(f"| {n} | {mn:.3f} | {mx:.3f} | {avg:.3f} | {std:.3f} |")
        a("")

    a("## Histograms (microseconds)\n")
    for idx, info in samples.items():
        a(f"### Step {idx}: `{info['label']}`\n")
        a("```")
        for lo, hi, c, bar in histogram(info["values"], bins=bins):
            a(f"[{lo:9.3f}, {hi:9.3f})  {c:5d} | {bar}")
        a("```")
        a("")
    return "\n".join(lines)


def print_console(samples, totals):
    print(f"Runs analyzed: {len(totals) if totals else 'n/a'}")
    print(f"{'Step':>4}  {'Count':>6}  {'Min':>10}  {'Max':>10}  "
          f"{'Avg':>10}  Event")
    for idx, info in samples.items():
        n, mn, mx, avg, _ = stats(info["values"])
        print(f"{idx:>4}  {n:>6}  {mn:>10.3f}  {mx:>10.3f}  "
              f"{avg:>10.3f}  {info['label']}")
    if totals:
        n, mn, mx, avg, _ = stats(totals)
        print(f"{'TOT':>4}  {n:>6}  {mn:>10.3f}  {mx:>10.3f}  {avg:>10.3f}  "
              f"end-to-end")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", default="-",
                    help="perf-script trace file (default: stdin)")
    ap.add_argument("-o", "--output", default="echo_trace_stats.md",
                    help="Markdown report path (default: echo_trace_stats.md)")
    ap.add_argument("-b", "--bins", type=int, default=10,
                    help="histogram bins (default: 10)")
    args = ap.parse_args()

    if args.input == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.input) as f:
            lines = f.readlines()

    runs = split_runs(parse_events(lines))
    if not runs:
        sys.exit("error: no runs found (no cmd_submit_enter events parsed)")

    samples, totals = aggregate(runs)
    print_console(samples, totals)

    report = build_report(samples, totals, args.bins)
    with open(args.output, "w") as f:
        f.write(report)
    print(f"\nWrote {args.output}")


if __name__ == "__main__":
    main()
