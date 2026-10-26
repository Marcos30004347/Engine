if(NOT DEFINED SAMPLE)
  message(FATAL_ERROR "SAMPLE is required")
endif()

set(TEST_FRAMES 4)
if(DEFINED FRAME_STABILITY AND FRAME_STABILITY)
  set(TEST_FRAMES 8)
endif()
# Defaults to SVRaster: that compares the two-phase per-voxel occlusion path
# against itself with occlusion off. Pass RESOLVER=bricks for the brick
# hierarchy's own per-brick occlusion path, which is the sample's default
# resolver and had no equivalence coverage at all - which is how an occlusion
# test that silently refused to judge 57% of what it drew went unnoticed.
if(NOT DEFINED RESOLVER)
  set(RESOLVER svraster)
endif()
set(COMMON_ARGS --gbuffer-resolver ${RESOLVER} --frames ${TEST_FRAMES} --enable-occlusion --enable-blas-occlusion --no-terrain)
set(REFERENCE_ARGS --gbuffer-resolver ${RESOLVER} --frames ${TEST_FRAMES} --disable-occlusion --no-terrain)
# An interior view is the only one with real depth complexity; culling on the
# open roof-down and preview cameras barely engages, so a test that used them
# would pass whether or not occlusion worked.
if(DEFINED SCENE_OBJ AND SCENE_OBJ)
  list(APPEND COMMON_ARGS --test-obj-preview "${SCENE_OBJ}" --voxel-size-meters 5
       --lod-threshold 1.5 --test-camera-interior --brick-measure-area)
  list(APPEND REFERENCE_ARGS --test-obj-preview "${SCENE_OBJ}" --voxel-size-meters 5
       --lod-threshold 1.5 --test-camera-interior --brick-measure-area)
endif()
if(DEFINED DEPTH_ONLY AND DEPTH_ONLY)
  list(APPEND COMMON_ARGS --depth-only)
  list(APPEND REFERENCE_ARGS --depth-only)
endif()
if(DEFINED DISOCCLUSION AND DISOCCLUSION)
  list(APPEND COMMON_ARGS --test-occlusion-disocclusion)
  list(APPEND REFERENCE_ARGS --test-occlusion-disocclusion)
endif()
if(DEFINED FRAME_STABILITY AND FRAME_STABILITY)
  list(APPEND COMMON_ARGS --test-camera-yaw --test-frame-signatures)
  list(APPEND REFERENCE_ARGS --test-camera-yaw --test-frame-signatures)
endif()
if(DEFINED BENCHMARK_TIMINGS AND BENCHMARK_TIMINGS)
  list(APPEND COMMON_ARGS --gpu-timings)
  list(APPEND REFERENCE_ARGS --gpu-timings)
endif()

execute_process(
  COMMAND "${SAMPLE}" ${COMMON_ARGS}
  RESULT_VARIABLE ENABLED_RESULT
  OUTPUT_VARIABLE ENABLED_STDOUT
  ERROR_VARIABLE ENABLED_STDERR
)
if(NOT ENABLED_RESULT EQUAL 0)
  message(FATAL_ERROR "Occlusion-enabled renderer failed:\n${ENABLED_STDOUT}\n${ENABLED_STDERR}")
endif()

execute_process(
  COMMAND "${SAMPLE}" ${REFERENCE_ARGS}
  RESULT_VARIABLE REFERENCE_RESULT
  OUTPUT_VARIABLE REFERENCE_STDOUT
  ERROR_VARIABLE REFERENCE_STDERR
)
if(NOT REFERENCE_RESULT EQUAL 0)
  message(FATAL_ERROR "Occlusion-disabled reference renderer failed:\n${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
endif()

set(ENABLED_LOG "${ENABLED_STDOUT}\n${ENABLED_STDERR}")
set(REFERENCE_LOG "${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
string(REGEX MATCH "coveredPixels=([0-9]+)" ENABLED_MATCH "${ENABLED_LOG}")
set(ENABLED_PIXELS "${CMAKE_MATCH_1}")
string(REGEX MATCH "coveredPixels=([0-9]+)" REFERENCE_MATCH "${REFERENCE_LOG}")
set(REFERENCE_PIXELS "${CMAKE_MATCH_1}")
if(ENABLED_PIXELS STREQUAL "" OR REFERENCE_PIXELS STREQUAL "")
  message(FATAL_ERROR "Could not parse renderer coverage:\n${ENABLED_LOG}\n${REFERENCE_LOG}")
endif()
if(NOT ENABLED_PIXELS STREQUAL REFERENCE_PIXELS)
  message(FATAL_ERROR "Occlusion changed final coverage: enabled=${ENABLED_PIXELS}, reference=${REFERENCE_PIXELS}")
endif()

string(REGEX MATCH "outputHash=([0-9a-fA-F]+)" ENABLED_HASH_MATCH "${ENABLED_LOG}")
set(ENABLED_HASH "${CMAKE_MATCH_1}")
string(REGEX MATCH "outputHash=([0-9a-fA-F]+)" REFERENCE_HASH_MATCH "${REFERENCE_LOG}")
set(REFERENCE_HASH "${CMAKE_MATCH_1}")
if(ENABLED_HASH STREQUAL "" OR REFERENCE_HASH STREQUAL "")
  message(FATAL_ERROR "Could not parse renderer output hashes")
endif()
set(OCCLUSION_HASH_MISMATCH OFF)
if(NOT ENABLED_HASH STREQUAL REFERENCE_HASH)
  set(OCCLUSION_HASH_MISMATCH ON)
endif()

if(DEFINED FRAME_STABILITY AND FRAME_STABILITY)
  string(REGEX MATCHALL "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+ outputHash=[0-9a-fA-F]+" ENABLED_SIGNATURES "${ENABLED_LOG}")
  string(REGEX MATCHALL "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+ outputHash=[0-9a-fA-F]+" REFERENCE_SIGNATURES "${REFERENCE_LOG}")
  list(LENGTH ENABLED_SIGNATURES ENABLED_SIGNATURE_COUNT)
  if(NOT ENABLED_SIGNATURE_COUNT EQUAL TEST_FRAMES)
    message(FATAL_ERROR "Expected ${TEST_FRAMES} per-frame occlusion signatures, got ${ENABLED_SIGNATURE_COUNT}")
  endif()
  if(NOT ENABLED_SIGNATURES STREQUAL REFERENCE_SIGNATURES)
    message(FATAL_ERROR "Occlusion changed an intermediate camera-angle frame:\n${ENABLED_SIGNATURES}\nreference:\n${REFERENCE_SIGNATURES}")
  endif()
endif()

string(REGEX MATCH "selected(Legacy)?LODNodes=([0-9]+)" ENABLED_WORK_MATCH "${ENABLED_LOG}")
set(ENABLED_WORK "${CMAKE_MATCH_2}")
string(REGEX MATCH "selected(Legacy)?LODNodes=([0-9]+)" REFERENCE_WORK_MATCH "${REFERENCE_LOG}")
set(REFERENCE_WORK "${CMAKE_MATCH_2}")
if(ENABLED_WORK STREQUAL "" OR REFERENCE_WORK STREQUAL "")
  message(FATAL_ERROR "Could not parse selected-node work counts")
endif()
if(ENABLED_WORK GREATER REFERENCE_WORK)
  message(FATAL_ERROR "Occlusion increased selected-node work: enabled=${ENABLED_WORK}, reference=${REFERENCE_WORK}")
endif()

if(OCCLUSION_HASH_MISMATCH)
  message(STATUS
    "Occlusion coverage/work matched before exact-output check: "
    "coveredPixels=${ENABLED_PIXELS}, enabledHash=${ENABLED_HASH}, "
    "referenceHash=${REFERENCE_HASH}, selected=${ENABLED_WORK}/${REFERENCE_WORK}")
else()
  message(STATUS
    "Occlusion equivalence passed without false negatives: "
    "coveredPixels=${ENABLED_PIXELS}, outputHash=${ENABLED_HASH}, "
    "selected=${ENABLED_WORK}/${REFERENCE_WORK}")
endif()

string(REGEX MATCH
       "Sparse voxel CPU frame summary: samples=([0-9]+) average=([0-9.]+)ms median=([0-9.]+)ms p95=([0-9.]+)ms"
       ENABLED_CPU_MATCH "${ENABLED_LOG}")
set(ENABLED_CPU_SAMPLES "${CMAKE_MATCH_1}")
set(ENABLED_CPU_AVERAGE "${CMAKE_MATCH_2}")
set(ENABLED_CPU_MEDIAN "${CMAKE_MATCH_3}")
set(ENABLED_CPU_P95 "${CMAKE_MATCH_4}")
string(REGEX MATCH
       "Sparse voxel CPU frame summary: samples=([0-9]+) average=([0-9.]+)ms median=([0-9.]+)ms p95=([0-9.]+)ms"
       REFERENCE_CPU_MATCH "${REFERENCE_LOG}")
set(REFERENCE_CPU_SAMPLES "${CMAKE_MATCH_1}")
set(REFERENCE_CPU_AVERAGE "${CMAKE_MATCH_2}")
set(REFERENCE_CPU_MEDIAN "${CMAKE_MATCH_3}")
set(REFERENCE_CPU_P95 "${CMAKE_MATCH_4}")
if(ENABLED_CPU_AVERAGE STREQUAL "" OR REFERENCE_CPU_AVERAGE STREQUAL "")
  message(FATAL_ERROR "Could not parse CPU frame summaries")
endif()
message(STATUS
  "Occlusion CPU frame summary: enabled samples=${ENABLED_CPU_SAMPLES} "
  "avg=${ENABLED_CPU_AVERAGE}ms median=${ENABLED_CPU_MEDIAN}ms "
  "p95=${ENABLED_CPU_P95}ms; reference samples=${REFERENCE_CPU_SAMPLES} "
  "avg=${REFERENCE_CPU_AVERAGE}ms median=${REFERENCE_CPU_MEDIAN}ms "
  "p95=${REFERENCE_CPU_P95}ms")

if(DEFINED BENCHMARK_TIMINGS AND BENCHMARK_TIMINGS)
  string(REGEX MATCH
         "Sparse voxel GPU timeline: samples=([0-9]+) timedPassSum=([0-9.]+)ms timestampSpan=([0-9.]+)ms compute=([0-9.]+)ms graphics=([0-9.]+)ms"
         ENABLED_GPU_MATCH "${ENABLED_LOG}")
  set(ENABLED_GPU_SAMPLES "${CMAKE_MATCH_1}")
  set(ENABLED_GPU_SUM "${CMAKE_MATCH_2}")
  set(ENABLED_GPU_SPAN "${CMAKE_MATCH_3}")
  set(ENABLED_GPU_COMPUTE "${CMAKE_MATCH_4}")
  set(ENABLED_GPU_GRAPHICS "${CMAKE_MATCH_5}")
  string(REGEX MATCH
         "Sparse voxel GPU timeline: samples=([0-9]+) timedPassSum=([0-9.]+)ms timestampSpan=([0-9.]+)ms compute=([0-9.]+)ms graphics=([0-9.]+)ms"
         REFERENCE_GPU_MATCH "${REFERENCE_LOG}")
  set(REFERENCE_GPU_SAMPLES "${CMAKE_MATCH_1}")
  set(REFERENCE_GPU_SUM "${CMAKE_MATCH_2}")
  set(REFERENCE_GPU_SPAN "${CMAKE_MATCH_3}")
  set(REFERENCE_GPU_COMPUTE "${CMAKE_MATCH_4}")
  set(REFERENCE_GPU_GRAPHICS "${CMAKE_MATCH_5}")
  if(ENABLED_GPU_SUM STREQUAL "" OR REFERENCE_GPU_SUM STREQUAL "")
    message(FATAL_ERROR "Could not parse GPU timelines")
  endif()
  string(REGEX MATCH
         "Sparse voxel GPU phases: temporal=([0-9.]+)ms provisionalHZB=([0-9.]+)ms actual=([0-9.]+)ms finalHZB=([0-9.]+)ms readback=([0-9.]+)ms other=([0-9.]+)ms"
         ENABLED_PHASE_MATCH "${ENABLED_LOG}")
  set(ENABLED_PHASE_TEMPORAL "${CMAKE_MATCH_1}")
  set(ENABLED_PHASE_PROVISIONAL_HZB "${CMAKE_MATCH_2}")
  set(ENABLED_PHASE_ACTUAL "${CMAKE_MATCH_3}")
  set(ENABLED_PHASE_FINAL_HZB "${CMAKE_MATCH_4}")
  set(ENABLED_PHASE_READBACK "${CMAKE_MATCH_5}")
  set(ENABLED_PHASE_OTHER "${CMAKE_MATCH_6}")
  string(REGEX MATCH
         "Sparse voxel GPU phases: temporal=([0-9.]+)ms provisionalHZB=([0-9.]+)ms actual=([0-9.]+)ms finalHZB=([0-9.]+)ms readback=([0-9.]+)ms other=([0-9.]+)ms"
         REFERENCE_PHASE_MATCH "${REFERENCE_LOG}")
  set(REFERENCE_PHASE_TEMPORAL "${CMAKE_MATCH_1}")
  set(REFERENCE_PHASE_PROVISIONAL_HZB "${CMAKE_MATCH_2}")
  set(REFERENCE_PHASE_ACTUAL "${CMAKE_MATCH_3}")
  set(REFERENCE_PHASE_FINAL_HZB "${CMAKE_MATCH_4}")
  set(REFERENCE_PHASE_READBACK "${CMAKE_MATCH_5}")
  set(REFERENCE_PHASE_OTHER "${CMAKE_MATCH_6}")
  if(ENABLED_PHASE_ACTUAL STREQUAL "" OR REFERENCE_PHASE_ACTUAL STREQUAL "")
    message(FATAL_ERROR "Could not parse GPU phase summaries")
  endif()
  message(STATUS
    "Occlusion GPU timeline: enabled samples=${ENABLED_GPU_SAMPLES} "
    "sum=${ENABLED_GPU_SUM}ms span=${ENABLED_GPU_SPAN}ms "
    "compute=${ENABLED_GPU_COMPUTE}ms graphics=${ENABLED_GPU_GRAPHICS}ms; "
    "reference samples=${REFERENCE_GPU_SAMPLES} sum=${REFERENCE_GPU_SUM}ms "
    "span=${REFERENCE_GPU_SPAN}ms compute=${REFERENCE_GPU_COMPUTE}ms "
    "graphics=${REFERENCE_GPU_GRAPHICS}ms")
  message(STATUS
    "Occlusion GPU phases: enabled temporal=${ENABLED_PHASE_TEMPORAL}ms "
    "provisionalHZB=${ENABLED_PHASE_PROVISIONAL_HZB}ms "
    "actual=${ENABLED_PHASE_ACTUAL}ms finalHZB=${ENABLED_PHASE_FINAL_HZB}ms "
    "readback=${ENABLED_PHASE_READBACK}ms other=${ENABLED_PHASE_OTHER}ms; "
    "reference temporal=${REFERENCE_PHASE_TEMPORAL}ms "
    "provisionalHZB=${REFERENCE_PHASE_PROVISIONAL_HZB}ms "
    "actual=${REFERENCE_PHASE_ACTUAL}ms finalHZB=${REFERENCE_PHASE_FINAL_HZB}ms "
    "readback=${REFERENCE_PHASE_READBACK}ms other=${REFERENCE_PHASE_OTHER}ms")
endif()

# The equivalence above only means something if occlusion actually removed
# something. A test that compares two identical renders passes forever, which is
# exactly the failure mode this project keeps rediscovering - so require the
# enabled run to have drawn strictly fewer bricks than the reference.
if(DEFINED SCENE_OBJ AND SCENE_OBJ AND RESOLVER STREQUAL "bricks")
  string(REGEX MATCH
         "brick cull: phase0=([0-9]+) phase1=([0-9]+) visible=([0-9]+) occluded=([0-9]+) overflow=([0-9]+) nodes=([0-9]+) visibleAreaPixels=([0-9]+)"
         ENABLED_VISIBLE_MATCH "${ENABLED_LOG}")
  set(ENABLED_PHASE0 "${CMAKE_MATCH_1}")
  set(ENABLED_PHASE1 "${CMAKE_MATCH_2}")
  set(ENABLED_VISIBLE "${CMAKE_MATCH_3}")
  set(ENABLED_OCCLUDED "${CMAKE_MATCH_4}")
  set(ENABLED_BRICK_OVERFLOW "${CMAKE_MATCH_5}")
  set(ENABLED_NODES "${CMAKE_MATCH_6}")
  set(ENABLED_AREA_PIXELS "${CMAKE_MATCH_7}")
  string(REGEX MATCH
         "brick cull: phase0=([0-9]+) phase1=([0-9]+) visible=([0-9]+) occluded=([0-9]+) overflow=([0-9]+) nodes=([0-9]+) visibleAreaPixels=([0-9]+)"
         REFERENCE_VISIBLE_MATCH "${REFERENCE_LOG}")
  set(REFERENCE_PHASE0 "${CMAKE_MATCH_1}")
  set(REFERENCE_PHASE1 "${CMAKE_MATCH_2}")
  set(REFERENCE_VISIBLE "${CMAKE_MATCH_3}")
  set(REFERENCE_OCCLUDED "${CMAKE_MATCH_4}")
  set(REFERENCE_BRICK_OVERFLOW "${CMAKE_MATCH_5}")
  set(REFERENCE_NODES "${CMAKE_MATCH_6}")
  set(REFERENCE_AREA_PIXELS "${CMAKE_MATCH_7}")
  if(ENABLED_VISIBLE STREQUAL "" OR REFERENCE_VISIBLE STREQUAL "")
    message(FATAL_ERROR "Could not parse brick cull counts:\n${ENABLED_LOG}")
  endif()
  if(NOT ENABLED_BRICK_OVERFLOW EQUAL 0 OR NOT REFERENCE_BRICK_OVERFLOW EQUAL 0)
    message(FATAL_ERROR
      "Brick culling overflowed: enabled=${ENABLED_BRICK_OVERFLOW}, "
      "reference=${REFERENCE_BRICK_OVERFLOW}")
  endif()
  if(NOT ENABLED_VISIBLE LESS REFERENCE_VISIBLE)
    message(FATAL_ERROR
      "Brick occlusion culled nothing, so the equivalence above is vacuous: "
      "enabled drew ${ENABLED_VISIBLE}, reference drew ${REFERENCE_VISIBLE}")
  endif()
  if(NOT ENABLED_OCCLUDED GREATER 0)
    message(FATAL_ERROR "Brick occlusion reported zero depth rejections")
  endif()
  message(STATUS
    "Brick occlusion counters: enabled phase0=${ENABLED_PHASE0} "
    "phase1=${ENABLED_PHASE1} visible=${ENABLED_VISIBLE} "
    "occluded=${ENABLED_OCCLUDED} area=${ENABLED_AREA_PIXELS}px; "
    "reference phase0=${REFERENCE_PHASE0} phase1=${REFERENCE_PHASE1} "
    "visible=${REFERENCE_VISIBLE} occluded=${REFERENCE_OCCLUDED} "
    "area=${REFERENCE_AREA_PIXELS}px; nodes=${ENABLED_NODES}/${REFERENCE_NODES}; "
    "outputHash=${ENABLED_HASH}")
endif()

if(OCCLUSION_HASH_MISMATCH)
  message(FATAL_ERROR
    "Occlusion changed exact output signature: enabled=${ENABLED_HASH}, "
    "reference=${REFERENCE_HASH}")
endif()
