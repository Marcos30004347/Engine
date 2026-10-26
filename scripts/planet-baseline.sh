#!/bin/sh
# Phase-0 baseline harness for the planet generator.
#
# Runs the headless spherical generator REPEATS times per resolution and leaves
# one output directory per run. Repeats are not optional: a single number from
# this generator is not a measurement, and prior performance work on this engine
# was invalidated by a 3x run-to-run variance that nobody measured first.
#
# Usage: scripts/planet-baseline.sh <output-root> [repeats] [radius-metres]
set -e

ROOT=${1:?usage: planet-baseline.sh <output-root> [repeats] [radius-metres]}
REPEATS=${2:-3}
RADIUS=${3:-100000}
SEED=${SEED:-9127364512}
BINARY=${BINARY:-./build-release/samples/PolygonTerrainSample}

if [ ! -x "$BINARY" ]; then
  echo "missing $BINARY: build PolygonTerrainSample first" >&2
  exit 1
fi

mkdir -p "$ROOT"
for WIDTH in 2048 4096; do
  HEIGHT=$((WIDTH / 2))
  RUN=1
  while [ "$RUN" -le "$REPEATS" ]; do
    OUT="$ROOT/planet-${WIDTH}x${HEIGHT}-run${RUN}"
    echo "=== ${WIDTH}x${HEIGHT} run ${RUN}/${REPEATS} -> $OUT"
    "$BINARY" --headless --sphere \
      --planet-radius "$RADIUS" \
      --seed "$SEED" \
      --resolution "$WIDTH" \
      --output "$OUT" >"$ROOT/planet-${WIDTH}x${HEIGHT}-run${RUN}.log" 2>&1
    grep -E "^  (total|graph total|raster erosion)" "$ROOT/planet-${WIDTH}x${HEIGHT}-run${RUN}.log" || true
    RUN=$((RUN + 1))
  done
done
echo "=== done; profiles in $ROOT/*/planet-profile.json"
