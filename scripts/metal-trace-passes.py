#!/usr/bin/env python3
"""Per-pass GPU timings from an Instruments 'Metal System Trace'.

Vulkan timestamp queries bracket Vulkan passes, so a stall inside MoltenVK is
billed to whichever pass follows it. The renderer's own --gpu-timings output is
subject to exactly that and has been observed attributing 27 ms to a finalize
kernel that does almost no work. This reads Metal encoder intervals instead,
which are what the GPU actually executed.

    xcrun xctrace record --template 'Metal System Trace' \
      --output frame.trace --launch -- \
      ./build-release/tests/SparseVoxelRendererSample <args> --profile-passes
    scripts/metal-trace-passes.py frame.trace --skip-fraction 0.5

--profile-passes is required for attribution: it stops the render graph merging
passes into one command buffer, so each pass gets its own command buffer, and
the RHI names that buffer after the pass. MoltenVK forwards the name to
MTLCommandBuffer.label, which is the only string that reaches this table -
debug labels and debug groups do not appear in it at all. The extra submissions
mean a profiled frame time is not a shippable frame time, so read shares.

Encoder intervals (nesting depth >= 1) are the unit. Their enclosing
command-buffer intervals (depth 0) are deliberately not summed: they include
scheduling gaps, and adding both would double count.
"""

import argparse
import collections
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

SCHEMA = "metal-gpu-intervals"
PASS_PREFIXES = ("svr.", "TerrainGPUV2.", "TerrainGPUBuild.")


def export(trace):
    xpath = '/trace-toc/run[@number="1"]/data/table[@schema="%s"]' % SCHEMA
    result = subprocess.run(
        ["xcrun", "xctrace", "export", "--input", trace, "--xpath", xpath],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("xctrace export failed: %s" % result.stderr.strip())
    return result.stdout


def rows(document):
    """Yield each row as a column-name -> (text, fmt) mapping.

    xctrace writes every distinct value once with an id and refers back to it
    afterwards, so values only resolve against everything seen earlier. Element
    tags are engineering types and repeat within a row, so columns are matched
    positionally against the schema header rather than by tag.
    """
    root = ElementTree.fromstring(document)
    schema = root.find(".//schema")
    if schema is None:
        return
    columns = [column.findtext("mnemonic") or "" for column in schema.findall("col")]
    values = {}
    for row in root.iter("row"):
        record = {}
        for index, field in enumerate(row):
            reference = field.get("ref")
            if reference is not None:
                value = values.get(reference, ("", ""))
            else:
                value = (field.text or "", field.get("fmt") or "")
                if field.get("id") is not None:
                    values[field.get("id")] = value
            if index < len(columns):
                record[columns[index]] = value
        yield record


def passName(label):
    """The render-graph pass a labelled encoder belongs to, or None.

    A labelled encoder reads
      "svr.sample.Foo_svr.sample.Foo_Pipeline_0:vkCmdDispatch ComputeEncoder …"
    where the part before the first colon is the command-buffer label the RHI
    set. Anything without that prefix is not one of our passes - MetalFX's own
    command buffers and the driver's channel rows land there - and is reported
    as unattributed rather than folded in.
    """
    head = label.split(":", 1)[0].strip()
    if not head.startswith(PASS_PREFIXES):
        return None
    # Vulkan debug names reach Metal in both `<pass>_Pipeline_0` and
    # `<pass>.Pipeline_0` forms. Strip either before collapsing the duplicated
    # `<pass>_<pass>` label produced by the render graph.
    head = re.sub(
        r"(?:_|\.)(Pipeline|MaterialPipeline|[A-Za-z]*Pipeline)_\d+$",
        "", head)
    head = re.sub(r"_\d+$", "", head).rstrip("_")
    # The graph names a pipeline "<pass>_<pass>"; collapse the repeat.
    half = len(head) // 2
    if head[:half] and head[:half] == head[half + 1:]:
        head = head[:half]
    return head or None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--process", default="SparseVoxelRendererSample",
                        help="only count encoders from this process")
    parser.add_argument("--skip-fraction", type=float, default=0.0,
                        help="drop this leading fraction of the trace's time "
                             "span. Terrain streaming has not converged at the "
                             "start of a run, so 0.5 measures the settled scene")
    parser.add_argument("--frames", type=int, default=0,
                        help="optional: application frames in the retained "
                             "window, to print a per-frame figure alongside "
                             "the shares")
    parser.add_argument("--top", type=int, default=25)
    arguments = parser.parse_args()

    records = []
    for record in rows(export(arguments.trace)):
        # The dedicated `process` column resolves empty for GPU intervals; the
        # owning process is spelled inside the formatted label instead, as
        # "…      ( SparseVoxelRendererSample (1234) )  0x…".
        label = record.get("event-label", ("", ""))[1]
        if arguments.process and arguments.process not in label:
            continue
        try:
            start = float(record.get("start", ("", ""))[0])
            nanoseconds = float(record.get("duration", ("", ""))[0])
        except ValueError:
            continue
        depth = record.get("event-depth", ("", ""))[1]
        records.append((start, nanoseconds, depth, label.split("      (")[0]))

    if not records:
        sys.exit("no %s rows for process '%s'; was the run recorded with the "
                 "Metal System Trace template?" % (SCHEMA, arguments.process))

    first = min(start for start, _, _, _ in records)
    last = max(start for start, _, _, _ in records)
    threshold = first + (last - first) * max(0.0, min(arguments.skip_fraction, 0.99))

    attributed = collections.Counter()
    calls = collections.Counter()
    unattributed = collections.Counter()
    for start, nanoseconds, depth, label in records:
        if start < threshold:
            continue
        # Depth 0 is the enclosing command buffer; its encoders are depth >= 1.
        if depth == "0":
            continue
        name = passName(label)
        if name is None:
            unattributed[label.split(":", 1)[0][:44]] += nanoseconds
            continue
        attributed[name] += nanoseconds
        calls[name] += 1

    total = sum(attributed.values())
    other = sum(unattributed.values())
    if total == 0.0:
        sys.exit("no named passes found. Was the run given --profile-passes? "
                 "Without it the graph merges every pass into one command "
                 "buffer and only that buffer carries a name.")

    window = (last - threshold) / 1.0e9
    print("window %.2f s of %.2f s   encoder time: %.1f ms in named passes, "
          "%.1f ms unattributed (%.0f%% attributed)" % (
              window, (last - first) / 1.0e9, total / 1.0e6, other / 1.0e6,
              100.0 * total / max(total + other, 1.0)))
    if arguments.frames > 0:
        print("per-frame figures assume %d frames in the window" % arguments.frames)

    # A page build is intentionally split into many indirect stages. Ranking
    # only individual encoders makes every stage look cheap while hiding their
    # aggregate movement cost, so report the generator families first.
    generator_groups = collections.Counter()
    generator_calls = collections.Counter()
    for name, nanoseconds in attributed.items():
        if not name.startswith("TerrainGPUV2."):
            continue
        parts = name.split(".")
        group = ".".join(parts[:3]) if len(parts) >= 3 else name
        generator_groups[group] += nanoseconds
        generator_calls[group] += calls[name]
    if generator_groups:
        print("\nGPU terrain generator families:")
        print("%10s %10s %10s %8s  %s" % (
            "total ms", "ms/frame", "ms/call", "calls", "family"))
        for name, nanoseconds in generator_groups.most_common():
            call_count = generator_calls[name]
            per_frame = nanoseconds / 1.0e6 / arguments.frames \
                if arguments.frames > 0 else 0.0
            per_call = nanoseconds / 1.0e6 / max(call_count, 1)
            print("%10.3f %10.3f %10.4f %8d  %s" % (
                nanoseconds / 1.0e6, per_frame, per_call, call_count, name))
    print()
    header = "%9s %8s" % ("share", "calls") if arguments.frames <= 0 \
        else "%9s %10s %8s" % ("share", "ms/frame", "calls/f")
    print("%s  %s" % (header, "pass"))
    for name, nanoseconds in attributed.most_common(arguments.top):
        if arguments.frames > 0:
            print("%8.1f%% %10.3f %8.1f  %s" % (
                100.0 * nanoseconds / total,
                nanoseconds / 1.0e6 / arguments.frames,
                calls[name] / arguments.frames, name))
        else:
            print("%8.1f%% %8d  %s" % (
                100.0 * nanoseconds / total, calls[name], name))

    if unattributed:
        print("\nunattributed encoder time (not a render-graph pass):")
        for label, nanoseconds in unattributed.most_common(6):
            print("%13.1f ms  %s" % (nanoseconds / 1.0e6, label))


if __name__ == "__main__":
    main()
