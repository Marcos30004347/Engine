if(NOT DEFINED SAMPLE)
  message(FATAL_ERROR "SAMPLE is required")
endif()

# R1 is a representation split, so its acceptance gate compares the complete
# exact-output contract against the incumbent brick draw. The diagnostic scan
# is enabled only to read back preparation invariants; it is not a benchmark.
set(TEST_FRAMES 3)
set(COMMON_ARGS
  --gbuffer-resolver bricks
  --frames ${TEST_FRAMES}
  --no-terrain
  --test-micro-raster
  # Exact microgeometry is retired by default now. These suites exist to
  # compare the micro raster paths against each other, so they have to ask for
  # it explicitly or they compare two identical no-micro images.
  --brick-exact-micro-all
  --test-frame-signatures
  --resolution 320x180
  --disable-lighting
  --no-taa
  --disable-metalfx-upscaler
  --disable-frame-generation
  --disable-vsync
)

function(run_renderer PREFIX)
  execute_process(
    COMMAND "${SAMPLE}" ${COMMON_ARGS} ${ARGN}
    RESULT_VARIABLE RUN_RESULT
    OUTPUT_VARIABLE RUN_STDOUT
    ERROR_VARIABLE RUN_STDERR
  )
  if(NOT RUN_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R1 ${PREFIX} renderer failed:\n${RUN_STDOUT}\n${RUN_STDERR}")
  endif()
  set("${PREFIX}_LOG" "${RUN_STDOUT}\n${RUN_STDERR}" PARENT_SCOPE)
endfunction()

# The compact/residual split is opt-in: it is byte-identical to the incumbent
# draw but slower, so the sample defaults to the incumbent path. Both the
# reference and enabled arms therefore name their path explicitly.
run_renderer(REFERENCE --disable-brick-micro-raster)
run_renderer(ENABLED --brick-micro-raster --brick-micro-prepare-diagnostic)
run_renderer(OVERFLOW
  --brick-micro-raster
  --brick-micro-prepare-diagnostic
  --brick-micro-record-capacity 1)

function(parse_exact_contract LOG_VARIABLE PREFIX)
  string(REGEX MATCHALL "Sparse voxel exact output:[^\r\n]*"
    CONTRACT_LINES "${${LOG_VARIABLE}}")
  if(NOT CONTRACT_LINES)
    message(FATAL_ERROR
      "Could not find exact-output lines in R1 ${PREFIX} renderer log")
  endif()
  list(LENGTH CONTRACT_LINES CONTRACT_LINE_COUNT)
  list(GET CONTRACT_LINES -1 CONTRACT_LINE)
  string(REGEX MATCHALL
    "[A-Za-z][A-Za-z0-9_]*=(0[xX])?[0-9a-fA-F]+"
    CONTRACT_FIELDS "${CONTRACT_LINE}")
  set(CONTRACT_KEYS)
  foreach(FIELD IN LISTS CONTRACT_FIELDS)
    string(REGEX REPLACE "=.*$" "" KEY "${FIELD}")
    string(REGEX REPLACE "^[^=]+=" "" VALUE "${FIELD}")
    list(APPEND CONTRACT_KEYS "${KEY}")
    set("${PREFIX}_${KEY}" "${VALUE}" PARENT_SCOPE)
  endforeach()
  set("${PREFIX}_KEYS" "${CONTRACT_KEYS}" PARENT_SCOPE)
  set("${PREFIX}_LINE" "${CONTRACT_LINE}" PARENT_SCOPE)
  set("${PREFIX}_LINES" "${CONTRACT_LINES}" PARENT_SCOPE)
  set("${PREFIX}_LINE_COUNT" "${CONTRACT_LINE_COUNT}" PARENT_SCOPE)
endfunction()

parse_exact_contract(REFERENCE_LOG REFERENCE)
parse_exact_contract(ENABLED_LOG ENABLED)
parse_exact_contract(OVERFLOW_LOG OVERFLOW)

foreach(PREFIX IN ITEMS REFERENCE ENABLED OVERFLOW)
  if(NOT ${PREFIX}_LINE_COUNT EQUAL TEST_FRAMES)
    message(FATAL_ERROR
      "R1 ${PREFIX} emitted ${${PREFIX}_LINE_COUNT} exact-output frames; "
      "expected ${TEST_FRAMES}")
  endif()
endforeach()

if(NOT "${REFERENCE_LINES}" STREQUAL "${ENABLED_LINES}")
  message(FATAL_ERROR
    "R1 compact/residual raster changed exact output:\n"
    "incumbent=${REFERENCE_LINES}\nenabled=${ENABLED_LINES}")
endif()
if(NOT "${REFERENCE_LINES}" STREQUAL "${OVERFLOW_LINES}")
  message(FATAL_ERROR
    "R1 overflow fallback changed exact output:\n"
    "incumbent=${REFERENCE_LINES}\noverflow=${OVERFLOW_LINES}")
endif()

# Run an enabled/incumbent exact sweep without diagnostic shaders. Yaw changes
# the Phase0/Phase1 cut and stresses projected parent boundaries; the interior
# case locks the rule that a camera-containing brick remains wholly legacy.
function(compare_exact_sweep SWEEP_NAME SWEEP_FRAMES)
  set(SWEEP_ARGS
    --gbuffer-resolver bricks
    --frames ${SWEEP_FRAMES}
    --no-terrain
    --test-micro-raster
  # Exact microgeometry is retired by default now. These suites exist to
  # compare the micro raster paths against each other, so they have to ask for
  # it explicitly or they compare two identical no-micro images.
  --brick-exact-micro-all
    ${ARGN}
    --test-frame-signatures
    --resolution 320x180
    --disable-lighting
    --no-taa
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  )
  execute_process(
    COMMAND "${SAMPLE}" ${SWEEP_ARGS} --disable-brick-micro-raster
    RESULT_VARIABLE SWEEP_REFERENCE_RESULT
    OUTPUT_VARIABLE SWEEP_REFERENCE_STDOUT
    ERROR_VARIABLE SWEEP_REFERENCE_STDERR
  )
  if(NOT SWEEP_REFERENCE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R1 ${SWEEP_NAME} incumbent failed:\n"
      "${SWEEP_REFERENCE_STDOUT}\n${SWEEP_REFERENCE_STDERR}")
  endif()
  execute_process(
    COMMAND "${SAMPLE}" ${SWEEP_ARGS} --brick-micro-raster
    RESULT_VARIABLE SWEEP_ENABLED_RESULT
    OUTPUT_VARIABLE SWEEP_ENABLED_STDOUT
    ERROR_VARIABLE SWEEP_ENABLED_STDERR
  )
  if(NOT SWEEP_ENABLED_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R1 ${SWEEP_NAME} compact/residual renderer failed:\n"
      "${SWEEP_ENABLED_STDOUT}\n${SWEEP_ENABLED_STDERR}")
  endif()
  set(SWEEP_REFERENCE_LOG
    "${SWEEP_REFERENCE_STDOUT}\n${SWEEP_REFERENCE_STDERR}")
  set(SWEEP_ENABLED_LOG
    "${SWEEP_ENABLED_STDOUT}\n${SWEEP_ENABLED_STDERR}")
  string(REGEX MATCHALL "Sparse voxel exact output:[^\r\n]*"
    SWEEP_REFERENCE_LINES "${SWEEP_REFERENCE_LOG}")
  string(REGEX MATCHALL "Sparse voxel exact output:[^\r\n]*"
    SWEEP_ENABLED_LINES "${SWEEP_ENABLED_LOG}")
  list(LENGTH SWEEP_REFERENCE_LINES SWEEP_REFERENCE_LINE_COUNT)
  list(LENGTH SWEEP_ENABLED_LINES SWEEP_ENABLED_LINE_COUNT)
  if(NOT SWEEP_REFERENCE_LINE_COUNT EQUAL SWEEP_FRAMES OR
     NOT SWEEP_ENABLED_LINE_COUNT EQUAL SWEEP_FRAMES)
    message(FATAL_ERROR
      "R1 ${SWEEP_NAME} expected ${SWEEP_FRAMES} exact frames, got "
      "incumbent=${SWEEP_REFERENCE_LINE_COUNT}, "
      "enabled=${SWEEP_ENABLED_LINE_COUNT}")
  endif()
  if(NOT "${SWEEP_REFERENCE_LINES}" STREQUAL "${SWEEP_ENABLED_LINES}")
    message(FATAL_ERROR
      "R1 compact/residual raster changed ${SWEEP_NAME}:\n"
      "incumbent=${SWEEP_REFERENCE_LINES}\n"
      "enabled=${SWEEP_ENABLED_LINES}")
  endif()
endfunction()

set(YAW_FRAMES 8)
set(INTERIOR_FRAMES 3)
compare_exact_sweep("a grazing-yaw frame" ${YAW_FRAMES} --test-camera-yaw)
compare_exact_sweep("a camera-interior frame" ${INTERIOR_FRAMES}
  --test-camera-interior)

set(REQUIRED_EXACT_KEYS
  frame
  depthHash
  resolvedSurfaceHash
  surfaceIdentityHash
  temporalIdentityHash
  packedMaterialBottomHash
  microPayloadHash
  materialSampleAddressHash
  geometryNormalHash
  shadingNormalHash
  gbufferHash
  exactOutputHash
  finalOutputHash
)
list(LENGTH REQUIRED_EXACT_KEYS REQUIRED_EXACT_KEY_COUNT)
foreach(PREFIX IN ITEMS REFERENCE ENABLED OVERFLOW)
  list(LENGTH ${PREFIX}_KEYS KEY_COUNT)
  if(NOT KEY_COUNT EQUAL REQUIRED_EXACT_KEY_COUNT)
    message(FATAL_ERROR
      "R1 ${PREFIX} exact-output schema changed: ${${PREFIX}_LINE}")
  endif()
  foreach(KEY IN LISTS REQUIRED_EXACT_KEYS)
    list(FIND ${PREFIX}_KEYS "${KEY}" KEY_INDEX)
    if(KEY_INDEX EQUAL -1)
      message(FATAL_ERROR
        "R1 ${PREFIX} exact-output line is missing '${KEY}': ${${PREFIX}_LINE}")
    endif()
    if(NOT KEY STREQUAL "frame" AND "${${PREFIX}_${KEY}}" MATCHES "^0+$")
      message(FATAL_ERROR
        "R1 ${PREFIX} did not capture '${KEY}': ${${PREFIX}_LINE}")
    endif()
  endforeach()
endforeach()

# The compatibility switch must restore the original graph topology. These
# names identify every production pass in the enabled split, including R2's
# sparse ownership replay; the diagnostic Scan is deliberately not required.
set(R1_PRODUCTION_PASSES
  .BrickPhase0.MicroPrepareReset
  .BrickPhase0.MicroPrepareTransform
  .BrickPhase0.ResidualDraw
  .BrickPhase0.PreparedMicroDraw
  .BrickPhase0.PreparedTieReplayDraw
)
foreach(PASS_SUFFIX IN LISTS R1_PRODUCTION_PASSES)
  string(FIND "${REFERENCE_LOG}" "Registering pass" REFERENCE_HAS_REGISTRATION)
  string(FIND "${REFERENCE_LOG}" "${PASS_SUFFIX}" REFERENCE_HAS_PASS)
  if(NOT REFERENCE_HAS_REGISTRATION EQUAL -1 AND
     NOT REFERENCE_HAS_PASS EQUAL -1)
    message(FATAL_ERROR
      "R1 disabled graph unexpectedly registered ${PASS_SUFFIX}")
  endif()
  string(FIND "${ENABLED_LOG}" "${PASS_SUFFIX}" ENABLED_HAS_PASS)
  if(ENABLED_HAS_PASS EQUAL -1)
    message(FATAL_ERROR
      "R1 enabled graph did not register ${PASS_SUFFIX}")
  endif()
endforeach()
string(FIND "${REFERENCE_LOG}" ".BrickPhase0.Draw" REFERENCE_HAS_FULL_DRAW)
if(REFERENCE_HAS_FULL_DRAW EQUAL -1)
  message(FATAL_ERROR "R1 disabled graph did not restore BrickPhase0.Draw")
endif()

function(parse_prepare_counters LOG_VARIABLE PREFIX)
  string(REGEX MATCHALL
    "Sparse voxel MicroPrepare unique parents:[^\r\n]*"
    UNIQUE_LINES "${${LOG_VARIABLE}}")
  string(REGEX MATCHALL
    "Sparse voxel MicroPrepare transform:[^\r\n]*"
    TRANSFORM_LINES "${${LOG_VARIABLE}}")
  if(NOT UNIQUE_LINES OR NOT TRANSFORM_LINES)
    message(FATAL_ERROR "Could not parse R1 ${PREFIX} MicroPrepare counters")
  endif()
  list(GET UNIQUE_LINES -1 UNIQUE_LINE)
  list(GET TRANSFORM_LINES -1 TRANSFORM_LINE)

  string(REGEX MATCH
    "phase0Bricks=([0-9]+)[^\r\n]*occupied=([0-9]+)[^\r\n]*micro=([0-9]+)"
    UNIQUE_MATCH "${UNIQUE_LINE}")
  if(UNIQUE_MATCH STREQUAL "")
    message(FATAL_ERROR "Malformed R1 ${PREFIX} unique-parent counters: ${UNIQUE_LINE}")
  endif()
  set(PHASE0_BRICKS "${CMAKE_MATCH_1}")
  set(SOURCE_PARENTS "${CMAKE_MATCH_2}")
  set(MICRO_PARENTS "${CMAKE_MATCH_3}")

  string(REGEX MATCH
    "records=([0-9]+)/([0-9]+)[^\r\n]*extractedParents=([0-9]+)[^\r\n]*residualParents=([0-9]+)[^\r\n]*overflowBricks=([0-9]+)[^\r\n]*outputBytes=([0-9]+)"
    TRANSFORM_MATCH "${TRANSFORM_LINE}")
  if(TRANSFORM_MATCH STREQUAL "")
    message(FATAL_ERROR "Malformed R1 ${PREFIX} transform counters: ${TRANSFORM_LINE}")
  endif()
  set(RECORDS "${CMAKE_MATCH_1}")
  set(CAPACITY "${CMAKE_MATCH_2}")
  set(EXTRACTED "${CMAKE_MATCH_3}")
  set(RESIDUAL "${CMAKE_MATCH_4}")
  set(OVERFLOW_BRICKS "${CMAKE_MATCH_5}")
  set(OUTPUT_BYTES "${CMAKE_MATCH_6}")

  foreach(NAME IN ITEMS PHASE0_BRICKS SOURCE_PARENTS MICRO_PARENTS RECORDS
                        CAPACITY EXTRACTED RESIDUAL OVERFLOW_BRICKS OUTPUT_BYTES)
    set("${PREFIX}_${NAME}" "${${NAME}}" PARENT_SCOPE)
  endforeach()
endfunction()

parse_prepare_counters(ENABLED_LOG ENABLED_PREPARE)
parse_prepare_counters(OVERFLOW_LOG OVERFLOW_PREPARE)

if(ENABLED_PREPARE_PHASE0_BRICKS LESS 1 OR
   ENABLED_PREPARE_MICRO_PARENTS LESS 1 OR
   ENABLED_PREPARE_RECORDS LESS 1 OR
   ENABLED_PREPARE_RESIDUAL LESS 1)
  message(FATAL_ERROR
    "R1 fixture did not exercise both products: "
    "bricks=${ENABLED_PREPARE_PHASE0_BRICKS}, "
    "micro=${ENABLED_PREPARE_MICRO_PARENTS}, "
    "records=${ENABLED_PREPARE_RECORDS}, residual=${ENABLED_PREPARE_RESIDUAL}")
endif()
if(NOT ENABLED_PREPARE_RECORDS EQUAL ENABLED_PREPARE_EXTRACTED)
  message(FATAL_ERROR
    "R1 record/extracted counts differ: "
    "${ENABLED_PREPARE_RECORDS} vs ${ENABLED_PREPARE_EXTRACTED}")
endif()
math(EXPR ENABLED_PARENT_SUM
  "${ENABLED_PREPARE_RECORDS} + ${ENABLED_PREPARE_RESIDUAL}")
if(NOT ENABLED_PARENT_SUM EQUAL ENABLED_PREPARE_SOURCE_PARENTS)
  message(FATAL_ERROR
    "R1 parent conservation failed: source=${ENABLED_PREPARE_SOURCE_PARENTS}, "
    "records=${ENABLED_PREPARE_RECORDS}, residual=${ENABLED_PREPARE_RESIDUAL}")
endif()
math(EXPR ENABLED_EXPECTED_BYTES "32 * ${ENABLED_PREPARE_RECORDS}")
if(NOT ENABLED_EXPECTED_BYTES EQUAL ENABLED_PREPARE_OUTPUT_BYTES)
  message(FATAL_ERROR
    "R1 compact record byte count is inconsistent: "
    "expected=${ENABLED_EXPECTED_BYTES}, actual=${ENABLED_PREPARE_OUTPUT_BYTES}")
endif()
if(NOT ENABLED_PREPARE_OVERFLOW_BRICKS EQUAL 0)
  message(FATAL_ERROR
    "R1 ordinary fixture unexpectedly overflowed "
    "${ENABLED_PREPARE_OVERFLOW_BRICKS} bricks")
endif()

if(NOT OVERFLOW_PREPARE_CAPACITY EQUAL 1 OR
   OVERFLOW_PREPARE_OVERFLOW_BRICKS LESS 1)
  message(FATAL_ERROR
    "R1 capacity-1 fixture did not force overflow: "
    "capacity=${OVERFLOW_PREPARE_CAPACITY}, "
    "overflow=${OVERFLOW_PREPARE_OVERFLOW_BRICKS}")
endif()
if(NOT OVERFLOW_PREPARE_RECORDS EQUAL OVERFLOW_PREPARE_EXTRACTED)
  message(FATAL_ERROR
    "R1 overflow record/extracted counts differ: "
    "${OVERFLOW_PREPARE_RECORDS} vs ${OVERFLOW_PREPARE_EXTRACTED}")
endif()
math(EXPR OVERFLOW_PARENT_SUM
  "${OVERFLOW_PREPARE_RECORDS} + ${OVERFLOW_PREPARE_RESIDUAL}")
if(NOT OVERFLOW_PARENT_SUM EQUAL OVERFLOW_PREPARE_SOURCE_PARENTS)
  message(FATAL_ERROR
    "R1 overflow parent conservation failed: "
    "source=${OVERFLOW_PREPARE_SOURCE_PARENTS}, "
    "records=${OVERFLOW_PREPARE_RECORDS}, "
    "residual=${OVERFLOW_PREPARE_RESIDUAL}")
endif()
math(EXPR OVERFLOW_EXPECTED_BYTES "32 * ${OVERFLOW_PREPARE_RECORDS}")
if(NOT OVERFLOW_EXPECTED_BYTES EQUAL OVERFLOW_PREPARE_OUTPUT_BYTES)
  message(FATAL_ERROR
    "R1 overflow compact byte count is inconsistent: "
    "expected=${OVERFLOW_EXPECTED_BYTES}, actual=${OVERFLOW_PREPARE_OUTPUT_BYTES}")
endif()
if(NOT OVERFLOW_PREPARE_RESIDUAL GREATER ENABLED_PREPARE_RESIDUAL)
  message(FATAL_ERROR
    "R1 overflow did not restore rejected parents to residual occupancy: "
    "ordinary=${ENABLED_PREPARE_RESIDUAL}, overflow=${OVERFLOW_PREPARE_RESIDUAL}")
endif()

message(STATUS
  "R1 exact equivalence passed for ${REQUIRED_EXACT_KEY_COUNT} fields; "
  "yawFrames=${YAW_FRAMES}, interiorFrames=${INTERIOR_FRAMES}, "
  "records=${ENABLED_PREPARE_RECORDS}, residual=${ENABLED_PREPARE_RESIDUAL}, "
  "capacity1Records=${OVERFLOW_PREPARE_RECORDS}, "
  "overflowBricks=${OVERFLOW_PREPARE_OVERFLOW_BRICKS}")
