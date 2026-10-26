if(NOT DEFINED SAMPLE)
  message(FATAL_ERROR "SAMPLE is required")
endif()

# This is a correctness harness, not a benchmark. The diagnostic shader uses
# fragment atomics and intentionally perturbs timing, so this test only asks
# whether enabling the complete R0 counter set changes any exact output field.
set(TEST_FRAMES 3)
set(COMMON_ARGS
  --gbuffer-resolver bricks
  --frames ${TEST_FRAMES}
  --no-terrain
  --test-static-scene
  --test-frame-signatures
  --resolution 320x180
  --no-taa
  --disable-metalfx-upscaler
  --disable-frame-generation
  --disable-vsync
)

execute_process(
  COMMAND "${SAMPLE}" ${COMMON_ARGS}
  RESULT_VARIABLE REFERENCE_RESULT
  OUTPUT_VARIABLE REFERENCE_STDOUT
  ERROR_VARIABLE REFERENCE_STDERR
)
if(NOT REFERENCE_RESULT EQUAL 0)
  message(FATAL_ERROR
    "R0 shipping renderer failed:\n${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
endif()

execute_process(
  COMMAND "${SAMPLE}" ${COMMON_ARGS} --brick-raster-diagnostics
  RESULT_VARIABLE DIAGNOSTIC_RESULT
  OUTPUT_VARIABLE DIAGNOSTIC_STDOUT
  ERROR_VARIABLE DIAGNOSTIC_STDERR
)
if(NOT DIAGNOSTIC_RESULT EQUAL 0)
  message(FATAL_ERROR
    "R0 diagnostic renderer failed:\n${DIAGNOSTIC_STDOUT}\n${DIAGNOSTIC_STDERR}")
endif()

set(REFERENCE_LOG "${REFERENCE_STDOUT}\n${REFERENCE_STDERR}")
set(DIAGNOSTIC_LOG "${DIAGNOSTIC_STDOUT}\n${DIAGNOSTIC_STDERR}")

# Export every key/value from the exact-contract line into PREFIX_<key>. This
# deliberately discovers the complete emitted field set instead of maintaining
# a second comparison list that can silently omit a newly added contract hash.
function(parse_exact_contract LOG_VARIABLE PREFIX)
  string(REGEX MATCHALL "Sparse voxel exact output:[^\r\n]*"
    CONTRACT_LINES "${${LOG_VARIABLE}}")
  if(NOT CONTRACT_LINES)
    message(FATAL_ERROR
      "Could not find 'Sparse voxel exact output:' in ${PREFIX} renderer log")
  endif()
  list(LENGTH CONTRACT_LINES CONTRACT_LINE_COUNT)
  list(GET CONTRACT_LINES -1 CONTRACT_LINE)

  string(REGEX MATCHALL
    "[A-Za-z][A-Za-z0-9_]*=(0[xX])?[0-9a-fA-F]+"
    CONTRACT_FIELDS "${CONTRACT_LINE}")
  if(NOT CONTRACT_FIELDS)
    message(FATAL_ERROR
      "Exact-output line in ${PREFIX} log contains no key=value hashes: ${CONTRACT_LINE}")
  endif()

  set(CONTRACT_KEYS)
  foreach(FIELD IN LISTS CONTRACT_FIELDS)
    string(REGEX REPLACE "=.*$" "" KEY "${FIELD}")
    string(REGEX REPLACE "^[^=]+=" "" VALUE "${FIELD}")
    list(FIND CONTRACT_KEYS "${KEY}" EXISTING_KEY)
    if(NOT EXISTING_KEY EQUAL -1)
      message(FATAL_ERROR
        "Exact-output line in ${PREFIX} log repeats key '${KEY}': ${CONTRACT_LINE}")
    endif()
    list(APPEND CONTRACT_KEYS "${KEY}")
    set("${PREFIX}_${KEY}" "${VALUE}" PARENT_SCOPE)
  endforeach()
  set("${PREFIX}_KEYS" "${CONTRACT_KEYS}" PARENT_SCOPE)
  set("${PREFIX}_LINE" "${CONTRACT_LINE}" PARENT_SCOPE)
  set("${PREFIX}_LINES" "${CONTRACT_LINES}" PARENT_SCOPE)
  set("${PREFIX}_LINE_COUNT" "${CONTRACT_LINE_COUNT}" PARENT_SCOPE)
endfunction()

parse_exact_contract(REFERENCE_LOG REFERENCE)
parse_exact_contract(DIAGNOSTIC_LOG DIAGNOSTIC)

if(NOT REFERENCE_LINE_COUNT EQUAL TEST_FRAMES OR
   NOT DIAGNOSTIC_LINE_COUNT EQUAL TEST_FRAMES)
  message(FATAL_ERROR
    "Expected ${TEST_FRAMES} exact-output frames, got "
    "shipping=${REFERENCE_LINE_COUNT}, diagnostic=${DIAGNOSTIC_LINE_COUNT}")
endif()
if(NOT "${REFERENCE_LINES}" STREQUAL "${DIAGNOSTIC_LINES}")
  message(FATAL_ERROR
    "R0 diagnostics changed an intermediate exact-output frame:\n"
    "shipping=${REFERENCE_LINES}\ndiagnostic=${DIAGNOSTIC_LINES}")
endif()

# Freeze the complete R0 schema as well as its values. `finalOutputHash` is the
# post-presentation colour signature; the two broader hashes retain a quick
# aggregate while the component hashes identify the exact contract that moved.
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
foreach(KEY IN LISTS REQUIRED_EXACT_KEYS)
  list(FIND REFERENCE_KEYS "${KEY}" REFERENCE_KEY_INDEX)
  if(REFERENCE_KEY_INDEX EQUAL -1)
    message(FATAL_ERROR
      "R0 exact-output line is missing '${KEY}': ${REFERENCE_LINE}")
  endif()
  if(NOT KEY STREQUAL "frame")
    set(HASH_VALUE "${REFERENCE_${KEY}}")
    if(HASH_VALUE MATCHES "^0+$")
      message(FATAL_ERROR
        "R0 exact-output field '${KEY}' was not captured: ${REFERENCE_LINE}")
    endif()
  endif()
endforeach()

list(LENGTH REFERENCE_KEYS REFERENCE_KEY_COUNT)
list(LENGTH DIAGNOSTIC_KEYS DIAGNOSTIC_KEY_COUNT)
list(LENGTH REQUIRED_EXACT_KEYS REQUIRED_EXACT_KEY_COUNT)
if(NOT REFERENCE_KEY_COUNT EQUAL REQUIRED_EXACT_KEY_COUNT)
  message(FATAL_ERROR
    "R0 exact-output schema changed: expected ${REQUIRED_EXACT_KEY_COUNT} "
    "fields (${REQUIRED_EXACT_KEYS}), got ${REFERENCE_KEY_COUNT}: ${REFERENCE_LINE}")
endif()
if(NOT REFERENCE_KEY_COUNT EQUAL DIAGNOSTIC_KEY_COUNT)
  message(FATAL_ERROR
    "R0 exact-output field count changed with diagnostics: "
    "shipping=${REFERENCE_LINE}; diagnostic=${DIAGNOSTIC_LINE}")
endif()

foreach(KEY IN LISTS REFERENCE_KEYS)
  list(FIND DIAGNOSTIC_KEYS "${KEY}" DIAGNOSTIC_KEY_INDEX)
  if(DIAGNOSTIC_KEY_INDEX EQUAL -1)
    message(FATAL_ERROR
      "R0 diagnostics dropped exact-output field '${KEY}': ${DIAGNOSTIC_LINE}")
  endif()
  if(NOT REFERENCE_${KEY} STREQUAL DIAGNOSTIC_${KEY})
    message(FATAL_ERROR
      "R0 diagnostics changed exact-output field '${KEY}': "
      "shipping=${REFERENCE_${KEY}}, diagnostic=${DIAGNOSTIC_${KEY}}")
  endif()
endforeach()

# Require the diagnostic run to exercise each major aggregate counter family.
# The no-terrain fixture has no micro-material descriptors, so its micro-trace
# count may correctly be zero; MicroPrepare must still publish residual work.
string(REGEX MATCH
  "Sparse voxel brick cull:[^\r\n]*visible=([0-9]+)[^\r\n]*visibleAreaPixels=([0-9]+)"
  CULL_COUNTERS "${DIAGNOSTIC_LOG}")
if(CULL_COUNTERS STREQUAL "")
  message(FATAL_ERROR "Could not parse R0 brick cull counters")
endif()
set(VISIBLE_BRICKS "${CMAKE_MATCH_1}")
set(PROJECTED_AREA "${CMAKE_MATCH_2}")

string(REGEX MATCH
  "Sparse voxel brick overdraw: fragments=([0-9]+) coveredPixels=([0-9]+) average=([0-9]+[.][0-9]+) peak=([0-9]+)"
  OVERDRAW_COUNTERS "${DIAGNOSTIC_LOG}")
if(OVERDRAW_COUNTERS STREQUAL "")
  message(FATAL_ERROR "Could not parse R0 brick overdraw counters")
endif()
set(FRAGMENT_INVOCATIONS "${CMAKE_MATCH_1}")
set(COVERED_PIXELS "${CMAKE_MATCH_2}")
set(AVERAGE_OVERDRAW "${CMAKE_MATCH_3}")
set(PEAK_OVERDRAW "${CMAKE_MATCH_4}")

string(REGEX MATCH
  "Sparse voxel brick traversal: scale4=[(]tested=([0-9]+) skipped=([0-9]+)[)] scale2Cells=([0-9]+) fineProbes=([0-9]+) firstHitExits=([0-9]+) misses=([0-9]+) carvedContinuations=([0-9]+)"
  TRAVERSAL_COUNTERS "${DIAGNOSTIC_LOG}")
if(TRAVERSAL_COUNTERS STREQUAL "")
  message(FATAL_ERROR "Could not parse R0 brick traversal counters")
endif()
set(SCALE4_TESTED "${CMAKE_MATCH_1}")
set(SCALE4_SKIPPED "${CMAKE_MATCH_2}")
set(SCALE2_CELLS "${CMAKE_MATCH_3}")
set(FINE_PROBES "${CMAKE_MATCH_4}")
set(FIRST_HIT_EXITS "${CMAKE_MATCH_5}")
set(TRAVERSAL_MISSES "${CMAKE_MATCH_6}")
set(CARVED_CONTINUATIONS "${CMAKE_MATCH_7}")

string(REGEX MATCH
  "Sparse voxel micro exact contexts: interior=([0-9]+) flat=([0-9]+) slope=([0-9]+) wall=([0-9]+) otherHeightfield=([0-9]+) generalVolume=([0-9]+) total=([0-9]+)"
  MICRO_CONTEXT_COUNTERS "${DIAGNOSTIC_LOG}")
if(MICRO_CONTEXT_COUNTERS STREQUAL "")
  message(FATAL_ERROR "Could not parse R0 micro-context counters")
endif()
set(MICRO_INTERIOR_TRACES "${CMAKE_MATCH_1}")
set(MICRO_FLAT_TRACES "${CMAKE_MATCH_2}")
set(MICRO_SLOPE_TRACES "${CMAKE_MATCH_3}")
set(MICRO_WALL_TRACES "${CMAKE_MATCH_4}")
set(MICRO_OTHER_HEIGHTFIELD_TRACES "${CMAKE_MATCH_5}")
set(MICRO_VOLUME_TRACES "${CMAKE_MATCH_6}")
set(MICRO_TRACES "${CMAKE_MATCH_7}")
math(EXPR MICRO_CONTEXT_AGGREGATE
  "${MICRO_INTERIOR_TRACES} + ${MICRO_FLAT_TRACES} + ${MICRO_SLOPE_TRACES} + ${MICRO_WALL_TRACES} + ${MICRO_OTHER_HEIGHTFIELD_TRACES} + ${MICRO_VOLUME_TRACES}")
if(NOT MICRO_CONTEXT_AGGREGATE EQUAL MICRO_TRACES)
  message(FATAL_ERROR
    "R0 micro-context total is inconsistent: "
    "contexts=${MICRO_CONTEXT_AGGREGATE}, total=${MICRO_TRACES}")
endif()

# MicroPrepare counters only exist when the opt-in R2 compact/residual split
# builds those passes. The exactness pair above deliberately stays on the
# default incumbent path, so the prepare counter suite gets its own arm rather
# than being dropped from R0's frozen instrumentation set.
execute_process(
  COMMAND "${SAMPLE}" ${COMMON_ARGS} --brick-raster-diagnostics
      --brick-micro-raster
  RESULT_VARIABLE MICRO_PREPARE_RESULT
  OUTPUT_VARIABLE MICRO_PREPARE_STDOUT
  ERROR_VARIABLE MICRO_PREPARE_STDERR
)
if(NOT MICRO_PREPARE_RESULT EQUAL 0)
  message(FATAL_ERROR
    "R0 MicroPrepare counter renderer failed:\n"
    "${MICRO_PREPARE_STDOUT}\n${MICRO_PREPARE_STDERR}")
endif()
set(MICRO_PREPARE_LOG "${MICRO_PREPARE_STDOUT}\n${MICRO_PREPARE_STDERR}")

string(REGEX MATCH
  "Sparse voxel MicroPrepare transform: records=([0-9]+)/[0-9]+[^\r\n]*residualParents=([0-9]+)[^\r\n]*overflowBricks=([0-9]+)[^\r\n]*outputBytes=([0-9]+)"
  MICRO_PREPARE_COUNTERS "${MICRO_PREPARE_LOG}")
if(MICRO_PREPARE_COUNTERS STREQUAL "")
  message(FATAL_ERROR "Could not parse R0 MicroPrepare counters")
endif()
set(MICRO_PREPARE_RECORDS "${CMAKE_MATCH_1}")
set(RESIDUAL_PARENTS "${CMAKE_MATCH_2}")
set(OVERFLOW_BRICKS "${CMAKE_MATCH_3}")
set(MICRO_PREPARE_OUTPUT_BYTES "${CMAKE_MATCH_4}")

if(VISIBLE_BRICKS LESS 1 OR PROJECTED_AREA LESS 1)
  message(FATAL_ERROR
    "R0 fixture did not exercise visible bricks/projected area: "
    "visible=${VISIBLE_BRICKS}, area=${PROJECTED_AREA}")
endif()
if(FRAGMENT_INVOCATIONS LESS 1 OR COVERED_PIXELS LESS 1 OR PEAK_OVERDRAW LESS 1)
  message(FATAL_ERROR
    "R0 fixture did not exercise fragment/coverage/overdraw counters: "
    "fragments=${FRAGMENT_INVOCATIONS}, covered=${COVERED_PIXELS}, peak=${PEAK_OVERDRAW}")
endif()
if(AVERAGE_OVERDRAW MATCHES "^0+([.]0+)?$")
  message(FATAL_ERROR
    "R0 fixture reported zero average overdraw: ${AVERAGE_OVERDRAW}")
endif()
if(FRAGMENT_INVOCATIONS LESS COVERED_PIXELS)
  message(FATAL_ERROR
    "R0 fragment count is smaller than covered pixels: "
    "fragments=${FRAGMENT_INVOCATIONS}, covered=${COVERED_PIXELS}")
endif()
if(SCALE4_TESTED LESS 1 OR SCALE4_SKIPPED LESS 1 OR
   SCALE2_CELLS LESS 1 OR FINE_PROBES LESS 1)
  message(FATAL_ERROR
    "R0 fixture did not exercise hierarchical traversal counters: "
    "scale4=${SCALE4_TESTED}/${SCALE4_SKIPPED}, "
    "scale2=${SCALE2_CELLS}, fine=${FINE_PROBES}")
endif()
math(EXPR HIT_MISS_CONTINUATION_AGGREGATE
  "${FIRST_HIT_EXITS} + ${TRAVERSAL_MISSES} + ${CARVED_CONTINUATIONS}")
if(HIT_MISS_CONTINUATION_AGGREGATE LESS 1)
  message(FATAL_ERROR
    "R0 fixture did not exercise hit/miss/continuation counters")
endif()
math(EXPR MICRO_PREPARE_AGGREGATE
  "${MICRO_PREPARE_RECORDS} + ${RESIDUAL_PARENTS} + ${OVERFLOW_BRICKS} + ${MICRO_PREPARE_OUTPUT_BYTES}")
if(MICRO_PREPARE_AGGREGATE LESS 1)
  message(FATAL_ERROR "R0 fixture did not exercise MicroPrepare counters")
endif()

math(EXPR RASTER_DIAGNOSTIC_AGGREGATE
  "${VISIBLE_BRICKS} + ${PROJECTED_AREA} + ${FRAGMENT_INVOCATIONS} + ${COVERED_PIXELS} + ${PEAK_OVERDRAW} + ${SCALE4_TESTED} + ${SCALE4_SKIPPED} + ${SCALE2_CELLS} + ${FINE_PROBES} + ${FIRST_HIT_EXITS} + ${TRAVERSAL_MISSES} + ${CARVED_CONTINUATIONS} + ${MICRO_TRACES} + ${MICRO_PREPARE_AGGREGATE}")
if(RASTER_DIAGNOSTIC_AGGREGATE LESS 1)
  message(FATAL_ERROR "R0 aggregate raster diagnostics are all zero")
endif()

message(STATUS
  "R0 exact equivalence passed for ${REFERENCE_KEY_COUNT} fields; "
  "diagnosticAggregate=${RASTER_DIAGNOSTIC_AGGREGATE}, "
  "fragments=${FRAGMENT_INVOCATIONS}, covered=${COVERED_PIXELS}, "
  "averageOverdraw=${AVERAGE_OVERDRAW}, "
  "microTraces=${MICRO_TRACES}, residualParents=${RESIDUAL_PARENTS}")
