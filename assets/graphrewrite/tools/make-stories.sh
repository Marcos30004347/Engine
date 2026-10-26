#!/bin/sh
# Regenerate the checked-in story sets.
#
#   assets/graphrewrite/tools/make-stories.sh [build-dir]
#
# Three sizes, all composed month by month by the chronicler (see
# docs/graphrewrite-chronicle-plan.md): ranked paragraphs, a closing count of
# what was too small to tell, and dialogue drawn from what has already
# happened to the people speaking.
#
# The village runs on the CPU - it is small enough that the whole event log
# fits and the CPU path is the one whose every firing is observable. The two
# large casts run on the GPU, where the composer runs inside the narration
# thread and never holds more than a month's budget. That is what makes two
# hundred thousand readable: listing every week produced thirty megabytes of
# prose for a single year.
#
# The cast files are generated, not checked in - make-cast.py is
# deterministic, so the same seed rebuilds the same world.
set -e

BUILD=${1:-build}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
STORY="$ROOT/$BUILD/src/cli/graphrewrite/graphrewrite-story"
RULES="$ROOT/assets/graphrewrite/story.graphwrite"
TOOLS="$ROOT/assets/graphrewrite/tools"
OUT="$ROOT/assets/graphrewrite/stories"
WORK=$(mktemp -d)

trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$STORY" ]; then
  echo "make-stories.sh: $STORY not built; cmake --build $BUILD --target graphrewrite-story" >&2
  exit 1
fi

# --- the village: the starting cast, on the CPU ------------------------------
echo "== village (32) =="
"$STORY" "$RULES" "$ROOT/assets/graphrewrite/story.graphfacts" \
    --seed 1 --seeds 4 --years 8 \
    --chronicle --per-month 6 --per-rule 2 --dialogues 2 \
    --out "$OUT/eight-years-chronicle"

# --- two hundred: one realm's worth, on the GPU ------------------------------
echo "== two hundred =="
python3 "$TOOLS/make-cast.py" --people 200 --seed 7 > "$WORK/cast-200.graphfacts"

"$STORY" "$RULES" "$WORK/cast-200.graphfacts" \
    --seed 7 --years 8 --gpu --rows 65536 \
    --chronicle --per-month 8 --per-rule 2 --dialogues 3 \
    --out "$OUT/two-hundred"

mv "$OUT/two-hundred/story-seed-7-gpu.txt" "$OUT/two-hundred/story-seed-7-gpu-chronicle.txt"

# --- two hundred thousand ----------------------------------------------------
# A bigger budget: a month of a world this size has more going on in it than a
# month of a village, and eight paragraphs would throw away the whole story to
# keep the arithmetic tidy. The summary line still accounts for the rest.
#
# The per-rule cap matters more than the budget here. Ranked on weight alone,
# a month of two hundred thousand people is twenty sudden illnesses - the
# heaviest rule there is, firing thousands of times. Two of any one rule
# leaves room for ten kinds of thing to happen.
echo "== two hundred thousand =="
python3 "$TOOLS/make-cast.py" --people 200000 --seed 9 > "$WORK/cast-200k.graphfacts"

"$STORY" "$RULES" "$WORK/cast-200k.graphfacts" \
    --seed 9 --years 1 --gpu --rows 6815744 --table-words 300000000 \
    --chronicle --per-month 20 --per-rule 2 --dialogues 4 \
    --out "$OUT/two-hundred-thousand"

mv "$OUT/two-hundred-thousand/story-seed-9-gpu.txt" \
   "$OUT/two-hundred-thousand/gpu-seed-9-chronicle.txt"

echo "done; see $OUT"
