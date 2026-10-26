#!/usr/bin/env python3
"""Summarize Xcode Metal System Trace exports (nanoseconds, not application timers).

Export metal-application-encoders-list, metal-gpu-intervals and optionally
metal-gpu-submission-to-command-buffer-id with xcrun xctrace export --xpath.
CPU values measure encoder lifetime, not GPU execution. GPU intervals are
joined to encoder IDs; overlapping intervals within an encoder are unioned.
"""
import argparse
import collections
import json
import math
import statistics
import xml.etree.ElementTree as ET


def rows(path):
    root = ET.parse(path).getroot()
    ids = {e.get("id"): e for e in root.iter() if e.get("id")}
    keys = [c.findtext("mnemonic") for c in root.find(".//schema").findall("col")]
    def value(element):
        if element.get("ref"):
            element = ids[element.get("ref")]
        return element.text or element.get("fmt", "")
    return [dict(zip(keys, map(value, row))) for row in root.findall(".//row")]


def distribution(values):
    ordered = sorted(values)
    return {"count": len(values), "total_ms": sum(values),
            "median_ms": statistics.median(values),
            "p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1],
            "max_ms": ordered[-1]}


def union_duration(intervals):
    total = 0
    start, end = sorted(intervals)[0]
    for left, right in sorted(intervals)[1:]:
        if left > end:
            total += end - start
            start, end = left, right
        else:
            end = max(end, right)
    return (total + end - start) / 1e6


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoders", required=True)
    parser.add_argument("--gpu", required=True)
    parser.add_argument("--submissions")
    parser.add_argument("--process", default="SparseVoxelRendererSample")
    parser.add_argument("--output", required=True)
    parser.add_argument("--isolated-passes", action="store_true",
                        help="Capture used --profile-passes with isolated RHI submissions")
    args = parser.parse_args()
    encoders = {r["encoder-id"]: r for r in rows(args.encoders)
                if r["process"].startswith(args.process + " (")}
    if not encoders:
        raise SystemExit("No target Metal encoders captured; launch under xctrace and check the target stayed alive.")
    submission_ids = {}
    if args.submissions:
        for row in rows(args.submissions):
            if row["encoder-id"] in encoders:
                submission_ids[row["gpu-submission-id"]] = row["encoder-id"]
    cpu = collections.defaultdict(list)
    for row in encoders.values():
        cpu[row["encoder-label"]].append(int(row["duration"]) / 1e6)
    intervals = collections.defaultdict(list)
    for row in rows(args.gpu):
        # Compute encoders can execute concurrently at nonzero display depths.
        # Union by encoder below instead of dropping those overlapping spans.
        if row["state"] != "Active":
            continue
        encoder_id = row["encoder-id"]
        if encoder_id not in encoders:
            encoder_id = submission_ids.get(row["gpu-submission-id"])
        if encoder_id not in encoders:
            continue
        start = int(row["start"])
        intervals[encoder_id].append((start, start + int(row["duration"])))
    gpu = collections.defaultdict(list)
    gpu_by_command = collections.defaultdict(list)
    for encoder_id, spans in intervals.items():
        encoder = encoders[encoder_id]
        duration = union_duration(spans)
        gpu[encoder["encoder-label"]].append(duration)
        gpu_by_command[encoder["cmdbuffer-label"] + " / " + encoder["encoder-label"]].append(duration)
    if not gpu:
        raise SystemExit("No GPU intervals matched the target encoders; check the submission export.")
    output = {
        "source": "Xcode Metal System Trace",
        "process": args.process,
        "units": "milliseconds",
        "notes": ["CPU encoder lifetime is separate from GPU execution.",
                  "GPU values union all active intervals per encoder, including overlapping display depths.",
                  "Encoders can overlap; totals are not whole-frame latency.",
                  ("Pass command buffers submitted separately for attribution; profiling adds submission overhead."
                   if args.isolated_passes else
                   "Deferred MoltenVK encoding can combine passes; command-buffer labels are grouping keys, not isolated kernel timings."),
                  "Streaming workloads and trace boundaries can change sample counts."],
        "cpu_encoder_lifetime": {k: distribution(v) for k, v in sorted(cpu.items())},
        "gpu_encoder_execution": {k: distribution(v) for k, v in sorted(gpu.items())},
        "gpu_by_command_buffer_label": {k: distribution(v) for k, v in sorted(gpu_by_command.items())},
    }
    with open(args.output, "w") as stream:
        json.dump(output, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
