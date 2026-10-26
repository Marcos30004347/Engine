if(NOT DEFINED SAMPLE)
  message(FATAL_ERROR "SAMPLE is required")
endif()

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

# Validate every frame, rather than only the final line. This catches both a
# contract-schema regression and a transient error hidden by a stable last
# frame before enabled/incumbent values are compared.
function(validate_exact_log LOG LABEL EXPECTED_FRAMES OUTPUT_LINES)
  string(REGEX MATCHALL "Sparse voxel exact output:[^\r\n]*"
    CONTRACT_LINES "${LOG}")
  list(LENGTH CONTRACT_LINES CONTRACT_LINE_COUNT)
  if(NOT CONTRACT_LINE_COUNT EQUAL EXPECTED_FRAMES)
    message(FATAL_ERROR
      "R2 ${LABEL} emitted ${CONTRACT_LINE_COUNT} exact-output frames; "
      "expected ${EXPECTED_FRAMES}")
  endif()

  set(EXPECTED_FRAME 0)
  foreach(CONTRACT_LINE IN LISTS CONTRACT_LINES)
    string(REGEX MATCHALL
      "[A-Za-z][A-Za-z0-9_]*=(0[xX])?[0-9a-fA-F]+"
      CONTRACT_FIELDS "${CONTRACT_LINE}")
    list(LENGTH CONTRACT_FIELDS CONTRACT_FIELD_COUNT)
    if(NOT CONTRACT_FIELD_COUNT EQUAL REQUIRED_EXACT_KEY_COUNT)
      message(FATAL_ERROR
        "R2 ${LABEL} exact-output schema changed: ${CONTRACT_LINE}")
    endif()

    set(CONTRACT_KEYS)
    set(ACTUAL_FRAME "")
    foreach(FIELD IN LISTS CONTRACT_FIELDS)
      string(REGEX REPLACE "=.*$" "" KEY "${FIELD}")
      string(REGEX REPLACE "^[^=]+=" "" VALUE "${FIELD}")
      list(APPEND CONTRACT_KEYS "${KEY}")
      if(KEY STREQUAL "frame")
        set(ACTUAL_FRAME "${VALUE}")
      elseif("${VALUE}" MATCHES "^(0[xX])?0+$")
        message(FATAL_ERROR
          "R2 ${LABEL} did not capture '${KEY}': ${CONTRACT_LINE}")
      endif()
    endforeach()
    foreach(KEY IN LISTS REQUIRED_EXACT_KEYS)
      list(FIND CONTRACT_KEYS "${KEY}" KEY_INDEX)
      if(KEY_INDEX EQUAL -1)
        message(FATAL_ERROR
          "R2 ${LABEL} exact-output line is missing '${KEY}': "
          "${CONTRACT_LINE}")
      endif()
    endforeach()
    if(NOT ACTUAL_FRAME STREQUAL "${EXPECTED_FRAME}")
      message(FATAL_ERROR
        "R2 ${LABEL} emitted frame=${ACTUAL_FRAME}; "
        "expected frame=${EXPECTED_FRAME}")
    endif()
    math(EXPR EXPECTED_FRAME "${EXPECTED_FRAME} + 1")
  endforeach()
  set("${OUTPUT_LINES}" "${CONTRACT_LINES}" PARENT_SCOPE)
endfunction()

# Run the incumbent and R2 direct/residual graph with identical deterministic
# controls. Only the two counter qualification cases select diagnostic shaders;
# the camera sweeps exercise the shipping binaries.
function(run_exact_case CASE_NAME FRAME_COUNT ENABLE_DIAGNOSTICS RECORD_CAPACITY)
  set(CASE_ARGS
    --gbuffer-resolver bricks
    --frames ${FRAME_COUNT}
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
    COMMAND "${SAMPLE}" ${CASE_ARGS} --disable-brick-micro-raster
    RESULT_VARIABLE REFERENCE_RESULT
    OUTPUT_VARIABLE REFERENCE_STDOUT
    ERROR_VARIABLE REFERENCE_STDERR
  )
  if(NOT REFERENCE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R2 ${CASE_NAME} incumbent failed:\n"
      "${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
  endif()

  # The split is opt-in; the sample now defaults to the incumbent draw because
  # R2 is byte-identical and measurably slower. Name the path explicitly.
  set(DIRECT_EXTRA_ARGS --brick-micro-raster)
  if(ENABLE_DIAGNOSTICS)
    list(APPEND DIRECT_EXTRA_ARGS --brick-raster-diagnostics)
  endif()
  if(RECORD_CAPACITY GREATER 0)
    list(APPEND DIRECT_EXTRA_ARGS
      --brick-micro-record-capacity ${RECORD_CAPACITY})
  endif()
  execute_process(
    COMMAND "${SAMPLE}" ${CASE_ARGS} ${DIRECT_EXTRA_ARGS}
    RESULT_VARIABLE DIRECT_RESULT
    OUTPUT_VARIABLE DIRECT_STDOUT
    ERROR_VARIABLE DIRECT_STDERR
  )
  if(NOT DIRECT_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R2 ${CASE_NAME} direct renderer failed:\n"
      "${DIRECT_STDOUT}\n${DIRECT_STDERR}")
  endif()

  set(REFERENCE_LOG "${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
  set(DIRECT_LOG "${DIRECT_STDOUT}\n${DIRECT_STDERR}")
  validate_exact_log(
    "${REFERENCE_LOG}" "${CASE_NAME} incumbent" ${FRAME_COUNT}
    REFERENCE_LINES)
  validate_exact_log(
    "${DIRECT_LOG}" "${CASE_NAME} direct" ${FRAME_COUNT}
    DIRECT_LINES)
  if(NOT "${REFERENCE_LINES}" STREQUAL "${DIRECT_LINES}")
    math(EXPR LAST_FRAME_INDEX "${FRAME_COUNT} - 1")
    foreach(FRAME_INDEX RANGE 0 ${LAST_FRAME_INDEX})
      list(GET REFERENCE_LINES ${FRAME_INDEX} REFERENCE_LINE)
      list(GET DIRECT_LINES ${FRAME_INDEX} DIRECT_LINE)
      if(NOT REFERENCE_LINE STREQUAL DIRECT_LINE)
        message(FATAL_ERROR
          "R2 direct/residual raster changed ${CASE_NAME} frame "
          "${FRAME_INDEX}:\nincumbent=${REFERENCE_LINE}\n"
          "direct=${DIRECT_LINE}")
      endif()
    endforeach()
    message(FATAL_ERROR
      "R2 direct/residual raster changed ${CASE_NAME} with no "
      "frame-local difference")
  endif()

  # The same three draws recorded into ONE render pass instead of three. This
  # measured neutral, so it is not the default, but it is a second ownership
  # ordering over the shared depth buffer and has to stay byte-identical.
  execute_process(
    COMMAND "${SAMPLE}" ${CASE_ARGS} ${DIRECT_EXTRA_ARGS}
            --brick-micro-merged-draw
    RESULT_VARIABLE MERGED_RESULT
    OUTPUT_VARIABLE MERGED_STDOUT
    ERROR_VARIABLE MERGED_STDERR
  )
  if(NOT MERGED_RESULT EQUAL 0)
    message(FATAL_ERROR
      "R2 ${CASE_NAME} merged renderer failed:\n"
      "${MERGED_STDOUT}\n${MERGED_STDERR}")
  endif()
  set(MERGED_LOG "${MERGED_STDOUT}\n${MERGED_STDERR}")
  validate_exact_log(
    "${MERGED_LOG}" "${CASE_NAME} merged" ${FRAME_COUNT} MERGED_LINES)
  if(NOT "${REFERENCE_LINES}" STREQUAL "${MERGED_LINES}")
    math(EXPR LAST_FRAME_INDEX "${FRAME_COUNT} - 1")
    foreach(FRAME_INDEX RANGE 0 ${LAST_FRAME_INDEX})
      list(GET REFERENCE_LINES ${FRAME_INDEX} REFERENCE_LINE)
      list(GET MERGED_LINES ${FRAME_INDEX} MERGED_LINE)
      if(NOT REFERENCE_LINE STREQUAL MERGED_LINE)
        message(FATAL_ERROR
          "R2 merged-encoder raster changed ${CASE_NAME} frame "
          "${FRAME_INDEX}:\nincumbent=${REFERENCE_LINE}\n"
          "merged=${MERGED_LINE}")
      endif()
    endforeach()
    message(FATAL_ERROR
      "R2 merged-encoder raster changed ${CASE_NAME} with no "
      "frame-local difference")
  endif()

  set("${CASE_NAME}_REFERENCE_LOG" "${REFERENCE_LOG}" PARENT_SCOPE)
  set("${CASE_NAME}_DIRECT_LOG" "${DIRECT_LOG}" PARENT_SCOPE)
  set("${CASE_NAME}_MERGED_LOG" "${MERGED_LOG}" PARENT_SCOPE)
endfunction()

run_exact_case(STATIC 3 TRUE 0)
run_exact_case(OVERFLOW 3 TRUE 1)
# R3's compact column cache only generates for staircase-surface parents, whose
# carve face is always +Y. The default framing looks along -Z from far along +Z,
# so no parent is ever entered through +Y there and the cache would stay at zero
# however many parents extract. The dolly camera starts directly above the model
# and is the only registered case that can observe it.
#
# Pinned to the historical handoff scale. The shipping default retires exact
# microgeometry at twice the projected size, which at the dolly framing retires
# every staircase parent and would leave this gate asserting against an empty
# population - it did exactly that when the default moved. The other cases below
# deliberately run at whatever the shipping default is.
run_exact_case(COLUMNS 3 TRUE 0 --test-camera-dolly --micro-handoff-scale 1.0)
run_exact_case(INTERIOR 3 FALSE 0 --test-camera-interior)
# Frame 0 is the far end and frame 60 the near end of the 120-frame cosine
# period, so this bounded half-cycle crosses the complete projected handoff
# band without repeating the return journey.
run_exact_case(DOLLY 61 FALSE 0 --test-camera-dolly)

function(parse_prepare_counters LOG PREFIX)
  string(REGEX MATCHALL
    "Sparse voxel MicroPrepare unique parents:[^\r\n]*"
    UNIQUE_LINES "${LOG}")
  string(REGEX MATCHALL
    "Sparse voxel MicroPrepare transform:[^\r\n]*"
    TRANSFORM_LINES "${LOG}")
  if(NOT UNIQUE_LINES OR NOT TRANSFORM_LINES)
    message(FATAL_ERROR "Could not parse R2 ${PREFIX} MicroPrepare counters")
  endif()
  list(GET UNIQUE_LINES -1 UNIQUE_LINE)
  list(GET TRANSFORM_LINES -1 TRANSFORM_LINE)
  string(REGEX MATCH
    "phase0Bricks=([0-9]+)[^\r\n]*occupied=([0-9]+)[^\r\n]*micro=([0-9]+)"
    UNIQUE_MATCH "${UNIQUE_LINE}")
  if(UNIQUE_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} unique-parent counters: ${UNIQUE_LINE}")
  endif()
  set(PHASE0_BRICKS "${CMAKE_MATCH_1}")
  set(SOURCE_PARENTS "${CMAKE_MATCH_2}")
  set(MICRO_PARENTS "${CMAKE_MATCH_3}")

  string(REGEX MATCH
    "records=([0-9]+)/([0-9]+)[^\r\n]*extractedParents=([0-9]+)[^\r\n]*residualParents=([0-9]+)[^\r\n]*overflowBricks=([0-9]+)[^\r\n]*outputBytes=([0-9]+)[^\r\n]*cachedRecords=([0-9]+)"
    TRANSFORM_MATCH "${TRANSFORM_LINE}")
  if(TRANSFORM_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} transform counters: ${TRANSFORM_LINE}")
  endif()
  set(RECORDS "${CMAKE_MATCH_1}")
  set(CAPACITY "${CMAKE_MATCH_2}")
  set(EXTRACTED "${CMAKE_MATCH_3}")
  set(RESIDUAL "${CMAKE_MATCH_4}")
  set(OVERFLOW_BRICKS "${CMAKE_MATCH_5}")
  set(OUTPUT_BYTES "${CMAKE_MATCH_6}")
  set(CACHED_RECORDS "${CMAKE_MATCH_7}")
  foreach(NAME IN ITEMS PHASE0_BRICKS SOURCE_PARENTS MICRO_PARENTS RECORDS
                        CAPACITY EXTRACTED RESIDUAL OVERFLOW_BRICKS OUTPUT_BYTES
                        CACHED_RECORDS)
    set("${PREFIX}_${NAME}" "${${NAME}}" PARENT_SCOPE)
  endforeach()
endfunction()

function(parse_raster_counters LOG PREFIX)
  string(REGEX MATCHALL "Sparse voxel micro carving:[^\r\n]*"
    CARVING_LINES "${LOG}")
  string(REGEX MATCHALL "Sparse voxel micro trace paths:[^\r\n]*"
    TRACE_LINES "${LOG}")
  string(REGEX MATCHALL "Sparse voxel brick traversal:[^\r\n]*"
    TRAVERSAL_LINES "${LOG}")
  string(REGEX MATCHALL "Sparse voxel brick overdraw:[^\r\n]*"
    OVERDRAW_LINES "${LOG}")
  if(NOT CARVING_LINES OR NOT TRACE_LINES OR NOT TRAVERSAL_LINES OR
     NOT OVERDRAW_LINES)
    message(FATAL_ERROR "Could not parse R2 ${PREFIX} raster counters")
  endif()
  list(GET CARVING_LINES -1 CARVING_LINE)
  list(GET TRACE_LINES -1 TRACE_LINE)
  list(GET TRAVERSAL_LINES -1 TRAVERSAL_LINE)
  list(GET OVERDRAW_LINES -1 OVERDRAW_LINE)

  string(REGEX MATCH
    "eligible=([0-9]+)[^\r\n]*descriptorLoads=([0-9]+)[^\r\n]*accepted=([0-9]+)[^\r\n]*carvedParents=([0-9]+)[^\r\n]*continuations=([0-9]+)[^\r\n]*finalDiscards=([0-9]+)[^\r\n]*exactTraces=([0-9]+)"
    CARVING_MATCH "${CARVING_LINE}")
  if(CARVING_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} carving counters: ${CARVING_LINE}")
  endif()
  set(ELIGIBLE "${CMAKE_MATCH_1}")
  set(DESCRIPTOR_LOADS "${CMAKE_MATCH_2}")
  set(ACCEPTED "${CMAKE_MATCH_3}")
  set(CARVED "${CMAKE_MATCH_4}")
  set(CONTINUATIONS "${CMAKE_MATCH_5}")
  set(FINAL_DISCARDS "${CMAKE_MATCH_6}")
  set(EXACT_TRACES "${CMAKE_MATCH_7}")

  string(REGEX MATCH
    "oneFace=\\(invocations=([0-9]+) steps=([0-9]+) hits=([0-9]+) misses=([0-9]+)\\)"
    TRACE_MATCH "${TRACE_LINE}")
  if(TRACE_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} trace counters: ${TRACE_LINE}")
  endif()
  set(ONE_FACE_INVOCATIONS "${CMAKE_MATCH_1}")
  set(ONE_FACE_STEPS "${CMAKE_MATCH_2}")
  set(ONE_FACE_HITS "${CMAKE_MATCH_3}")
  set(ONE_FACE_MISSES "${CMAKE_MATCH_4}")

  string(REGEX MATCH
    "scale4=\\(tested=([0-9]+) skipped=([0-9]+)\\)[^\r\n]*scale2Cells=([0-9]+)[^\r\n]*fineProbes=([0-9]+)"
    TRAVERSAL_MATCH "${TRAVERSAL_LINE}")
  if(TRAVERSAL_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} traversal counters: ${TRAVERSAL_LINE}")
  endif()
  set(SCALE4_TESTED "${CMAKE_MATCH_1}")
  set(SCALE4_SKIPPED "${CMAKE_MATCH_2}")
  set(SCALE2_CELLS "${CMAKE_MATCH_3}")
  set(FINE_PROBES "${CMAKE_MATCH_4}")

  string(REGEX MATCH "fragments=([0-9]+)[^\r\n]*coveredPixels=([0-9]+)"
    OVERDRAW_MATCH "${OVERDRAW_LINE}")
  if(OVERDRAW_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Malformed R2 ${PREFIX} overdraw counters: ${OVERDRAW_LINE}")
  endif()
  set(FRAGMENTS "${CMAKE_MATCH_1}")
  set(COVERED_PIXELS "${CMAKE_MATCH_2}")

  foreach(NAME IN ITEMS ELIGIBLE DESCRIPTOR_LOADS ACCEPTED CARVED CONTINUATIONS
                        FINAL_DISCARDS EXACT_TRACES ONE_FACE_INVOCATIONS
                        ONE_FACE_STEPS ONE_FACE_HITS ONE_FACE_MISSES
                        SCALE4_TESTED SCALE4_SKIPPED SCALE2_CELLS FINE_PROBES
                        FRAGMENTS COVERED_PIXELS)
    set("${PREFIX}_${NAME}" "${${NAME}}" PARENT_SCOPE)
  endforeach()
endfunction()

parse_prepare_counters("${STATIC_DIRECT_LOG}" STATIC_PREPARE)
parse_prepare_counters("${OVERFLOW_DIRECT_LOG}" OVERFLOW_PREPARE)
parse_prepare_counters("${COLUMNS_DIRECT_LOG}" COLUMNS_PREPARE)
parse_raster_counters("${STATIC_DIRECT_LOG}" STATIC_RASTER)
parse_raster_counters("${OVERFLOW_DIRECT_LOG}" OVERFLOW_RASTER)

if(STATIC_PREPARE_PHASE0_BRICKS LESS 1 OR
   STATIC_PREPARE_MICRO_PARENTS LESS 1 OR
   STATIC_PREPARE_RECORDS LESS 1 OR
   STATIC_PREPARE_RESIDUAL LESS 1)
  message(FATAL_ERROR
    "R2 fixture did not exercise prepared and residual products: "
    "bricks=${STATIC_PREPARE_PHASE0_BRICKS}, "
    "micro=${STATIC_PREPARE_MICRO_PARENTS}, "
    "records=${STATIC_PREPARE_RECORDS}, "
    "residual=${STATIC_PREPARE_RESIDUAL}")
endif()
if(NOT STATIC_PREPARE_RECORDS EQUAL STATIC_PREPARE_EXTRACTED)
  message(FATAL_ERROR "R2 prepared record/extracted counts differ")
endif()
math(EXPR STATIC_PARENT_SUM
  "${STATIC_PREPARE_RECORDS} + ${STATIC_PREPARE_RESIDUAL}")
if(NOT STATIC_PARENT_SUM EQUAL STATIC_PREPARE_SOURCE_PARENTS)
  message(FATAL_ERROR "R2 static parent conservation failed")
endif()
math(EXPR STATIC_EXPECTED_BYTES "32 * ${STATIC_PREPARE_RECORDS}")
if(NOT STATIC_EXPECTED_BYTES EQUAL STATIC_PREPARE_OUTPUT_BYTES)
  message(FATAL_ERROR "R2 static compact-record byte count is inconsistent")
endif()
if(NOT STATIC_PREPARE_OVERFLOW_BRICKS EQUAL 0)
  message(FATAL_ERROR "R2 static fixture unexpectedly overflowed")
endif()

# R3's compact column cache. Without a case that actually generates it, a
# deliberately corrupted cache leaves every exactness hash above unchanged -
# that is exactly how the feature shipped uncovered. The cached count must stay
# below the extracted count so the live-evaluation fallback is exercised too.
if(COLUMNS_PREPARE_CACHED_RECORDS LESS 1)
  message(FATAL_ERROR
    "R2 top-down case generated no R3 column caches, so no exactness gate "
    "constrains them: records=${COLUMNS_PREPARE_RECORDS}, "
    "cached=${COLUMNS_PREPARE_CACHED_RECORDS}")
endif()
if(COLUMNS_PREPARE_CACHED_RECORDS GREATER COLUMNS_PREPARE_RECORDS)
  message(FATAL_ERROR
    "R2 cached column records exceed extracted records: "
    "records=${COLUMNS_PREPARE_RECORDS}, "
    "cached=${COLUMNS_PREPARE_CACHED_RECORDS}")
endif()
if(NOT STATIC_PREPARE_CACHED_RECORDS EQUAL 0)
  message(FATAL_ERROR
    "R2 default framing unexpectedly extracted a staircase-surface parent; "
    "the top-down case is no longer the only column-cache observation: "
    "cached=${STATIC_PREPARE_CACHED_RECORDS}")
endif()

# These counters span residual, direct, and the sparse tie replay. Together
# with a nonzero prepared indirect instance count and the separately registered
# SPIR-V structural test, nonzero one-face work proves the direct draw is real,
# while fine probes prove the residual/ambiguity traversal is also real.
foreach(NAME IN ITEMS ELIGIBLE DESCRIPTOR_LOADS ACCEPTED CARVED CONTINUATIONS
                      FINAL_DISCARDS EXACT_TRACES ONE_FACE_INVOCATIONS
                      ONE_FACE_STEPS ONE_FACE_HITS ONE_FACE_MISSES SCALE4_TESTED
                      SCALE2_CELLS FINE_PROBES FRAGMENTS COVERED_PIXELS)
  if(STATIC_RASTER_${NAME} LESS 1)
    message(FATAL_ERROR "R2 static raster counter ${NAME} was zero")
  endif()
endforeach()

if(NOT OVERFLOW_PREPARE_CAPACITY EQUAL 1 OR
   OVERFLOW_PREPARE_OVERFLOW_BRICKS LESS 1)
  message(FATAL_ERROR
    "R2 capacity-1 case did not force whole-brick overflow: "
    "capacity=${OVERFLOW_PREPARE_CAPACITY}, "
    "overflow=${OVERFLOW_PREPARE_OVERFLOW_BRICKS}")
endif()
if(NOT OVERFLOW_PREPARE_RECORDS EQUAL OVERFLOW_PREPARE_EXTRACTED)
  message(FATAL_ERROR "R2 overflow record/extracted counts differ")
endif()
math(EXPR OVERFLOW_PARENT_SUM
  "${OVERFLOW_PREPARE_RECORDS} + ${OVERFLOW_PREPARE_RESIDUAL}")
if(NOT OVERFLOW_PARENT_SUM EQUAL OVERFLOW_PREPARE_SOURCE_PARENTS)
  message(FATAL_ERROR "R2 overflow parent conservation failed")
endif()
math(EXPR OVERFLOW_EXPECTED_BYTES "32 * ${OVERFLOW_PREPARE_RECORDS}")
if(NOT OVERFLOW_EXPECTED_BYTES EQUAL OVERFLOW_PREPARE_OUTPUT_BYTES)
  message(FATAL_ERROR "R2 overflow compact-record byte count is inconsistent")
endif()
if(NOT OVERFLOW_PREPARE_RECORDS LESS STATIC_PREPARE_RECORDS OR
   NOT OVERFLOW_PREPARE_RESIDUAL GREATER STATIC_PREPARE_RESIDUAL)
  message(FATAL_ERROR
    "R2 overflow did not move rejected bricks back to residual occupancy")
endif()
if(OVERFLOW_RASTER_EXACT_TRACES LESS 1 OR
   OVERFLOW_RASTER_FINE_PROBES LESS 1 OR
   OVERFLOW_RASTER_FRAGMENTS LESS 1)
  message(FATAL_ERROR "R2 overflow fallback raster counters were vacuous")
endif()
if(NOT STATIC_RASTER_EXACT_TRACES GREATER OVERFLOW_RASTER_EXACT_TRACES)
  message(FATAL_ERROR
    "R2 prepared records did not add observable direct exact-trace work")
endif()

set(R2_PRODUCTION_PASSES
  .BrickPhase0.MicroPrepareReset
  .BrickPhase0.MicroPrepareTransform
  .BrickPhase0.ResidualDraw
  .BrickPhase0.PreparedMicroDraw
  .BrickPhase0.PreparedTieReplayDraw
)
foreach(PASS_SUFFIX IN LISTS R2_PRODUCTION_PASSES)
  string(FIND "${STATIC_DIRECT_LOG}" "${PASS_SUFFIX}" DIRECT_HAS_PASS)
  if(DIRECT_HAS_PASS EQUAL -1)
    message(FATAL_ERROR "R2 graph did not register ${PASS_SUFFIX}")
  endif()
  string(FIND "${STATIC_REFERENCE_LOG}" "${PASS_SUFFIX}" REFERENCE_HAS_PASS)
  if(NOT REFERENCE_HAS_PASS EQUAL -1)
    message(FATAL_ERROR "R2 disabled graph unexpectedly registered ${PASS_SUFFIX}")
  endif()
endforeach()
string(FIND "${STATIC_REFERENCE_LOG}" ".BrickPhase0.Draw" REFERENCE_FULL_DRAW)
if(REFERENCE_FULL_DRAW EQUAL -1)
  message(FATAL_ERROR "R2 disabled graph did not restore BrickPhase0.Draw")
endif()

# The merged graph must collapse the three draw passes into one. Each pass
# records exactly one cmdBeginRenderPass, so this is what proves the option
# actually removes two render passes over the four-MRT brick target rather
# than only renaming them.
string(FIND "${STATIC_MERGED_LOG}" ".BrickPhase0.MergedMicroDraw"
  MERGED_HAS_PASS)
if(MERGED_HAS_PASS EQUAL -1)
  message(FATAL_ERROR "R2 merged graph did not register MergedMicroDraw")
endif()
foreach(PASS_SUFFIX IN ITEMS .BrickPhase0.ResidualDraw
                             .BrickPhase0.PreparedMicroDraw
                             .BrickPhase0.PreparedTieReplayDraw)
  string(FIND "${STATIC_MERGED_LOG}" "${PASS_SUFFIX}" MERGED_HAS_SPLIT_PASS)
  if(NOT MERGED_HAS_SPLIT_PASS EQUAL -1)
    message(FATAL_ERROR
      "R2 merged graph still registered ${PASS_SUFFIX}, so it did not merge "
      "the render passes")
  endif()
endforeach()

# Keep the rotating boundary case last so a failure still leaves the static,
# overflow, interior, dolly, and diagnostic qualification paths exercised.
run_exact_case(YAW 8 FALSE 0 --test-camera-yaw)

message(STATUS
  "R2 direct equivalence passed for ${REQUIRED_EXACT_KEY_COUNT} fields; "
  "static=3, yaw=8, interior=3, overflow=3, columns=3, dolly=61 frames; "
  "columnCacheRecords=${COLUMNS_PREPARE_CACHED_RECORDS}"
  "/${COLUMNS_PREPARE_RECORDS}, "
  "records=${STATIC_PREPARE_RECORDS}, residual=${STATIC_PREPARE_RESIDUAL}, "
  "directTraces=${STATIC_RASTER_EXACT_TRACES}, "
  "continuations=${STATIC_RASTER_CONTINUATIONS}, "
  "overflowBricks=${OVERFLOW_PREPARE_OVERFLOW_BRICKS}")
