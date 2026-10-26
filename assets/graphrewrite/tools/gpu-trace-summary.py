#!/usr/bin/env python3
"""Sum the GPU compute encoders of graphrewrite-story from a Metal System Trace.

    xcrun xctrace export --input run.trace \
        --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > gpu.xml
    tools/gpu-trace-summary.py gpu.xml

Groups by the encoder's Metal object label (the pass and pipeline name the RHI
gave it) and falls back to the command-buffer label. Values in the export are
id/ref pairs, so a row's cells are read by position against the schema.
"""
import collections
import sys
import xml.etree.ElementTree as ET


def to_ms(text):
    text = text.replace(",", "")
    for suffix, factor in (("ms", 1.0), ("µs", 1e-3), ("ns", 1e-6), ("s", 1e3)):
        if text.endswith(suffix):
            try:
                return float(text[: -len(suffix)].strip()) * factor
            except ValueError:
                return 0.0
    return 0.0


def main(path, process="graphrewrite"):
    root = ET.parse(path).getroot()
    cols = [c.find("mnemonic").text for c in root.find(".//schema").findall("col")]
    cache = {}

    def val(el):
        if "ref" in el.attrib:
            return cache.get(el.attrib["ref"], "")
        v = el.attrib.get("fmt", "")
        for sub in el.iter():
            if "id" in sub.attrib:
                cache[sub.attrib["id"]] = sub.attrib.get("fmt", "")
        return v

    total = collections.Counter()
    count = collections.Counter()
    gpu_ms = 0.0
    n = 0
    for row in root.iter("row"):
        cells = list(row)
        if len(cells) < len(cols):
            continue
        d = dict(zip(cols, [val(c) for c in cells]))
        if d["channel-name"] != "Compute" or process not in d["process"]:
            continue
        n += 1
        ms = to_ms(d["duration"])
        gpu_ms += ms
        key = d["channel-subtitle"] or d["event-label"].split("(")[0].strip()
        total[key] += ms
        count[key] += 1
    print("%s: %d compute encoders, %.1f ms on the GPU" % (process, n, gpu_ms))
    for key, ms in total.most_common(20):
        print("  %-60s n=%6d total %8.1f ms avg %.3f ms" % (key[:60], count[key], ms, ms / count[key]))


if __name__ == "__main__":
    main(sys.argv[1], *(sys.argv[2:3]))
