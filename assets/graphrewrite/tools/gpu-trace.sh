#!/bin/sh
# Capture a Metal System Trace of a short GPU story run for Instruments.
#
#   tools/gpu-trace.sh <cast.graphfacts> <weeks> <out.trace> [extra graphrewrite-story args]
#
# Open the .trace in Instruments (Metal System Trace) to see every compute
# encoder per frame on the GPU timeline. Keep runs short: a long trace drops
# events and reads as an idle GPU (see the project's perf notes).
set -e
CAST="$1"; WEEKS="$2"; OUT="$3"; shift 3
HERE="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$HERE/build/src/cli/graphrewrite/graphrewrite-story"
rm -rf "$OUT"
xcrun xctrace record --template 'Metal System Trace' --time-limit 60s --output "$OUT" \
  --launch -- "$BIN" "$HERE/assets/graphrewrite/story.graphwrite" "$CAST" \
  --weeks "$WEEKS" --gpu --no-text --out "${TMPDIR:-/tmp}/gpu-trace-out" "$@"
echo "trace: $OUT"
