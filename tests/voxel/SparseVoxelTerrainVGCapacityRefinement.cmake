# Terrain VG capacity refinement: the user's normal launch, bounded.
#
# The 5 km default-camera cut fills its resident chunk budget within a few
# hundred frames, and the two planner failures this test exists for only appear
# once it is full:
#
#   - the final cut overflows. Eight requested splits on 489 resident chunks
#     want 489 + 8 * 3 = 513 leaves against a 512 budget, and the whole
#     transaction was rejected with "exceeds its resident chunk budget",
#     claiming the cut had already been coarsened as far as it could be.
#   - the transition overflows. 486 resident chunks and eight splits give a
#     legal final cut of 510, but need 32 child slots while all 486 published
#     owners still hold theirs: 518 live at once. The allocator handed the
#     excess children an invalid slot and generation failed with "exhausted
#     its slots".
#
# Both retried the identical demand every frame forever, so the cut froze with
# every counter reporting activity. `limitWorldVoxelCutToCapacity` admits whole
# ownership groups in priority order against peak coexistence, so a batch that
# does not fit loses its last group rather than all of them. Neither message
# may appear, and the cut has to keep converging afterwards.
#
# This is the default camera deliberately: every other terrain VG contract uses
# a scripted or elevated camera, and the reported stall was on the plain spawn
# view. `--test-static-scene` removes the bounded-run instance animation, and
# the explicit threshold stops `--frames` selecting the historical 64 px test
# cut.
if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env SVR_TRACE_TERRAIN=1 "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 5
    --frames 300
    --memory-budget-mb 7168
    --resolution 640x360
    --lod-threshold 1.5
    --test-static-scene
    --test-frame-signatures
    --test-capture-start-frame 292
    --no-taa
    --jitter-scale 0
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  TIMEOUT 180
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)
set(LOG "${STDOUT}\n${STDERR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelTerrainVGCapacityRefinement.log" "${LOG}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Terrain VG capacity refinement run failed:\n${LOG}")
endif()

# Neither planner rejection may occur. These are not graceful back-pressure:
# the transaction is dropped whole, and the same demand is rebuilt and rejected
# on the next frame with nothing changed in between.
string(REGEX MATCHALL "exhausted its slots" SLOT_FAILURES "${LOG}")
list(LENGTH SLOT_FAILURES SLOT_FAILURE_COUNT)
if(SLOT_FAILURE_COUNT GREATER 0)
  message(FATAL_ERROR
    "The generated-world slot allocator ran out of slots ${SLOT_FAILURE_COUNT} "
    "times: a refinement batch was admitted whose published owners, staged "
    "owners and new children cannot coexist in the resident budget")
endif()
string(REGEX MATCHALL "exceeds its resident chunk budget" CUT_FAILURES "${LOG}")
list(LENGTH CUT_FAILURES CUT_FAILURE_COUNT)
if(CUT_FAILURE_COUNT GREATER 0)
  message(FATAL_ERROR
    "The desired spatial cut overflowed its resident chunk budget "
    "${CUT_FAILURE_COUNT} times: whole refinement batches are being rejected "
    "instead of admitting the groups that fit")
endif()

string(REGEX MATCH
  "Sparse voxel terrain settle: lastActivityFrame=([0-9]+) lod0Chunks=([0-9]+) residentChunks=([0-9]+) installFailures=([0-9]+) noSlice=([0-9]+)"
  SETTLE_LINE "${LOG}")
if(NOT SETTLE_LINE)
  message(FATAL_ERROR "No settle summary line in the run:\n${LOG}")
endif()
set(LOD0 "${CMAKE_MATCH_2}")
set(RESIDENT "${CMAKE_MATCH_3}")
set(INSTALL_FAILURES "${CMAKE_MATCH_4}")
set(NO_SLICE "${CMAKE_MATCH_5}")

# The failures above only bite at capacity, so a run that never filled the
# budget has not tested anything. The measured default run holds 486 of 512
# resident chunks by frame 240.
if(RESIDENT LESS 400)
  message(FATAL_ERROR
    "The 5 km cut did not approach its resident budget "
    "(residentChunks=${RESIDENT}); this run never reached the capacity the "
    "planner failures need and proves nothing: ${SETTLE_LINE}")
endif()
# Ground under the camera has to reach the finest spatial level. A cut that
# fills its budget with distant chunks and leaves the near field coarse is the
# stall this test is named for, wearing a healthy resident count.
if(LOD0 LESS 16)
  message(FATAL_ERROR
    "The ground under the camera did not reach the finest spatial level "
    "(lod0Chunks=${LOD0}) even though the cut is at capacity: ${SETTLE_LINE}")
endif()
if(NOT INSTALL_FAILURES EQUAL 0)
  message(FATAL_ERROR
    "Page or chunk installs failed for space (installFailures=${INSTALL_FAILURES}): ${SETTLE_LINE}")
endif()
if(NOT NO_SLICE EQUAL 0)
  message(FATAL_ERROR
    "Resident chunks without a hierarchy slice (noSlice=${NO_SLICE}): ${SETTLE_LINE}")
endif()

# Require multiple successful generation transactions, rather than merely a
# fully populated bootstrap cut.
string(REGEX MATCHALL "World terrain GPU commit: generation=([0-9]+)" COMMIT_LINES "${LOG}")
list(LENGTH COMMIT_LINES COMMIT_COUNT)
if(COMMIT_COUNT LESS 10)
  message(FATAL_ERROR
    "The generated-world streamer published only ${COMMIT_COUNT} transactions "
    "over 300 frames: the refinement feed is not converging the cut")
endif()

# A capacity-blocked cut must wait for a changed camera or publication. The
# old scheduler kept launching the same generation with built=0 indefinitely.
string(REGEX MATCHALL "World terrain no-op:" NO_OP_LINES "${LOG}")
list(LENGTH NO_OP_LINES NO_OP_COUNT)
math(EXPR NO_OP_LIMIT "${COMMIT_COUNT} + 1")
if(NO_OP_COUNT GREATER NO_OP_LIMIT)
  message(FATAL_ERROR
    "The scheduler retried unchanged work: no-ops=${NO_OP_COUNT}, commits=${COMMIT_COUNT}")
endif()

# Source generation must prepare the ground in every camera quadrant,
# including the quadrants outside the current view.
string(REGEX MATCHALL "World terrain chunk footprint:[^\n]*" FOOTPRINTS "${LOG}")
if(NOT FOOTPRINTS)
  message(FATAL_ERROR "No radial source-cut diagnostics")
endif()
list(GET FOOTPRINTS -1 FOOTPRINT)
string(REGEX MATCH "lod0Quadrants=\\[([0-9]+),([0-9]+),([0-9]+),([0-9]+)\\]" QUADRANTS "${FOOTPRINT}")
if(NOT QUADRANTS OR CMAKE_MATCH_1 LESS 1 OR CMAKE_MATCH_2 LESS 1 OR
   CMAKE_MATCH_3 LESS 1 OR CMAKE_MATCH_4 LESS 1)
  message(FATAL_ERROR "10 cm source terrain is not prepared around the camera: ${FOOTPRINT}")
endif()

# Nominal chunk counts alone missed the original bug: obsolete ancestor
# geometry covered the fine chunks. Check rendered error and near-field LOD.
string(REGEX MATCHALL "Sparse voxel terrain lod profile:[^\n]*" PROFILES "${LOG}")
if(NOT PROFILES)
  message(FATAL_ERROR "No terrain LOD profile in the run")
endif()
list(GET PROFILES -1 PROFILE)
if(NOT PROFILE MATCHES "nearCoarse=0 " OR NOT PROFILE MATCHES "v=0.10")
  message(FATAL_ERROR "The visible near field did not reach 10 cm: ${PROFILE}")
endif()
string(REGEX MATCHALL "Sparse voxel terrain VG cut contract:[^\n]*" CUTS "${LOG}")
if(NOT CUTS)
  message(FATAL_ERROR "No rendered terrain cut diagnostics")
endif()
list(GET CUTS -1 CUT)
string(REGEX MATCH "maxStreamingLeafErrorPx=([0-9.]+|inf)" ERROR_MATCH "${CUT}")
if(NOT ERROR_MATCH OR CMAKE_MATCH_1 STREQUAL "inf" OR CMAKE_MATCH_1 GREATER 4.0)
  message(FATAL_ERROR "Oversized fallback geometry remains in the rendered cut: ${CUT}")
endif()
string(REGEX MATCHALL "Sparse voxel frame signature:[^\n]*" SIGNATURES "${LOG}")
if(NOT SIGNATURES)
  message(FATAL_ERROR "No captured terrain coverage signature")
endif()
list(GET SIGNATURES -1 SIGNATURE)
string(REGEX MATCH "enclosed=([0-9]+)" ENCLOSED "${SIGNATURE}")
if(NOT ENCLOSED OR NOT CMAKE_MATCH_1 EQUAL 0)
  message(FATAL_ERROR "The settled terrain has an enclosed geometry gap: ${SIGNATURE}")
endif()
message(STATUS
  "Terrain VG capacity refinement passed: ${SETTLE_LINE}; "
  "commits=${COMMIT_COUNT}; ${PROFILE}; ${CUT}")
