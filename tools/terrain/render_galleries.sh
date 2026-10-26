#!/usr/bin/env bash
# Run the three generator galleries and render them.
#
#   tools/terrain/render_galleries.sh [build-dir] [out-dir]
#
# Each gallery is a test: it asserts the invariants that must hold whatever the
# shape, and writes the geometry out so the result can also be looked at. The
# assertions catch a generator that is wrong; the renders catch one that is
# merely bad, which is the failure numbers do not show.
set -euo pipefail

BUILD="${1:-build}"
OUT="${2:-$BUILD/tests/gallery}"
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$OUT"

run() {
  local name="$1" columns="$2"
  "$BUILD/tests/${name}Tests" "$OUT/${name}.json"
  python3 "$HERE/render_generator_gallery.py" "$OUT/${name}.json" "$OUT/${name}.png" --columns "$columns"
}

run GeneratorGallery 4
run WorldGallery 2
run IntegrationGallery 3

echo "galleries in $OUT"
