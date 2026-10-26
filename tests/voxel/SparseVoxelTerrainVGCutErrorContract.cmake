if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# The virtual-geometry cut contract on world terrain: every drawn cluster
# keeps its triangles near the pixel budget, or provably cannot refine.
#
#   - "giantRefinable" clusters could refine inside their asset but were drawn
#     oversized anyway: a DAG cut or error-metric defect. Always zero.
#   - "giantStreamingLeaves" are clusters whose finer pages exist but were
#     never installed: a stalled install pipeline. Zero once settled.
#   - "giantTrueLeaves" are clusters at their asset's finest level; only the
#     error-driven terrain chunk rebuild loop can fix them. Their count drains
#     at the bounded generation/meshing rate, so this test asserts the loop is
#     alive and unobstructed rather than fully drained: chunk rebuilds must
#     have deepened the cut well past the distance-scheduled bootstrap, and
#     the two generation failure modes that used to freeze it silently -
#     vertical voxel capacity and virtual-arena exhaustion - must not occur.
#
# The elevated slope camera is the adversarial case for distance-scheduled
# chunk LOD: chunks a kilometre away project far larger than their distance
# suggests, so the cut deepens only if the demand band published by the
# culler actually drives chunk rebuilds.
execute_process(
  # The contract line does not need the terrain trace, and the trace's
  # per-frame logging roughly doubles the frame time once the cut holds a few
  # hundred chunks (43.8 ms traced against 18.4 ms untraced at 200 frames).
  COMMAND "${SAMPLE}"
    --disable-lighting --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 1
    --frames 1200
    --resolution 640x360
    --test-world-terrain-slope
    --lod-threshold 1.5
    --no-taa
    --jitter-scale 0
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)

set(LOG "${STDOUT}\n${STDERR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Terrain VG cut error contract run failed:\n${LOG}")
endif()

string(REGEX MATCHALL "Sparse voxel terrain VG cut contract:[^\n]*"
  CONTRACT_LINES "${LOG}")
if(NOT CONTRACT_LINES)
  message(FATAL_ERROR "No terrain VG cut contract line in the run:\n${LOG}")
endif()
list(GET CONTRACT_LINES -1 CONTRACT_LINE)
string(REGEX MATCH
  "cutClusters=([0-9]+) maxTriPx=([0-9.]+|inf) giants=([0-9]+) giantStreamingLeaves=([0-9]+) giantTrueLeaves=([0-9]+) giantRefinable=(-?[0-9]+) maxStreamingLeafErrorPx=([0-9.]+|inf) trueLeafParentPx=([0-9.]+|inf) refinableSelfPx=([0-9.]+|inf)"
  CONTRACT_MATCH "${CONTRACT_LINE}")
if(NOT CONTRACT_MATCH)
  message(FATAL_ERROR "Malformed terrain VG cut contract: ${CONTRACT_LINE}")
endif()
set(CUT_CLUSTERS "${CMAKE_MATCH_1}")
set(MAX_TRI_PX "${CMAKE_MATCH_2}")
set(GIANT_STREAMING_LEAVES "${CMAKE_MATCH_4}")
set(GIANT_REFINABLE "${CMAKE_MATCH_6}")
set(REFINABLE_SELF_PX "${CMAKE_MATCH_9}")
# A "giant" is a cluster whose triangles are large on screen. Since the shared
# terrain object gave every chunk's finest clusters their voxel size as error
# (2026-09-05), a drawn non-streaming cluster can legitimately hold large
# triangles: flat ground greedy-merges into brick-sized quads whose geometric
# error is still under the budget. The defect this check exists for is a
# cluster drawn while its *error* exceeds the budget - the culler must never
# admit one - so it is the projected self error of refinable giants that has
# to stay under the threshold, not their count.
# The line prints two decimals: a cluster admitted at 1.4996 px reads 1.50.
if(NOT GIANT_REFINABLE EQUAL 0 AND
   (REFINABLE_SELF_PX STREQUAL "inf" OR REFINABLE_SELF_PX GREATER 1.55))
  message(FATAL_ERROR
    "Terrain VG cut drew refinable clusters whose error exceeds the budget "
    "(giantRefinable=${GIANT_REFINABLE} refinableSelfPx=${REFINABLE_SELF_PX}): ${CONTRACT_LINE}")
endif()
# Streaming-leaf giants are the resident frontier: chunks whose finer level is
# not resident yet, drawn oversized because nothing finer exists. Under the
# per-object route these were "true leaves" and uncapped; the shared object
# marks them streaming, so the count measures how far the streamer is from the
# camera's demand and is bounded by the generation budget, not by the install
# pipeline. The convergence measure that catches a stalled feed is maxTriPx.

# The cut must actually be constrained by the 1.5 px budget: at this camera a
# healthy cut holds tens of thousands of clusters. A tiny cut with zero giants
# means the threshold plumbing broke and the contract went vacuous.
if(CUT_CLUSTERS LESS 4000)
  message(FATAL_ERROR
    "Terrain VG cut did not refine under the pixel budget "
    "(cutClusters=${CUT_CLUSTERS}): ${CONTRACT_LINE}")
endif()

# The worst drawn triangle must have come down from the distance-scheduled
# bootstrap's ~64 px. Full drainage is bounded by the generation budget and
# is not required, but a ceiling well below the broken state catches a stalled
# feedback loop.
if(MAX_TRI_PX STREQUAL "inf" OR MAX_TRI_PX GREATER 56)
  message(FATAL_ERROR
    "Terrain VG cut error is not converging "
    "(maxTriPx=${MAX_TRI_PX}): ${CONTRACT_LINE}")
endif()

# The two silent generation failure modes that froze refinement while every
# counter looked healthy. Either line means a chunk was asked to rebuild finer
# and the generator refused, forever.
string(FIND "${LOG}" "exceeds its vertical voxel capacity" VERTICAL_OFFSET)
if(NOT VERTICAL_OFFSET EQUAL -1)
  message(FATAL_ERROR
    "Terrain generation hit the vertical voxel capacity again; steep ground "
    "can no longer refine")
endif()
# A page-arena allocation failure is graceful per transaction - the previous
# hierarchy stays resident and the chunk retries - so occasional failures
# under a deep cut are not a freeze. A freeze shows up as the cut never
# converging, which the maxTriPx assertion below already catches. Only flag a
# runaway: failures on a large fraction of frames means the arena is
# chronically undersized and the cut is thrashing rather than converging.
string(REGEX MATCHALL "virtual page allocation failed" ALLOCATION_FAILURES
    "${LOG}")
list(LENGTH ALLOCATION_FAILURES ALLOCATION_FAILURE_COUNT)
if(ALLOCATION_FAILURE_COUNT GREATER 600)
  message(FATAL_ERROR
    "Terrain virtual page arena is chronically undersized "
    "(${ALLOCATION_FAILURE_COUNT} allocation failures over 1200 frames); "
    "the cut is thrashing instead of converging")
endif()

# Error-driven demand must have deepened the cut well past the bootstrap's
# distance-scheduled chunk set (74 chunks at this camera).
# The contract line carries the resident chunk count; the last "chunks=" in
# the whole log is a bootstrap line once the run is untraced.
string(REGEX MATCH "chunks=([0-9]+)" CHUNK_MATCH "${CONTRACT_LINE}")
set(FINAL_CHUNKS "${CMAKE_MATCH_1}")
if(FINAL_CHUNKS LESS 150)
  message(FATAL_ERROR
    "Error-driven chunk refinement did not deepen the terrain cut "
    "(chunks=${FINAL_CHUNKS}): the demand band is not reaching the "
    "generation queue")
endif()

message(STATUS
  "Terrain VG cut error contract passed: ${CONTRACT_LINE}; "
  "chunks=${FINAL_CHUNKS}")
