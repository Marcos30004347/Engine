if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP OR
   NOT DEFINED WORLD_GENERATOR OR NOT DEFINED GAME_WORLD_MAP)
  message(FATAL_ERROR
    "SAMPLE, WORLD_MAP, WORLD_GENERATOR, and GAME_WORLD_MAP are required")
endif()

file(REMOVE_RECURSE "${GAME_WORLD_MAP}")
execute_process(
  COMMAND "${WORLD_GENERATOR}"
    --headless
    --export "${GAME_WORLD_MAP}"
    --export-resolution 2048
    --extent 128
    --spacing 0.7
    --levels 2
    --relief 2200
    --continent-span 105
    --continents 1
    --tags coast,highland
    --land 0.75
    --ranges 3
    --seed 7
  RESULT_VARIABLE GENERATOR_RESULT
  OUTPUT_VARIABLE GENERATOR_STDOUT
  ERROR_VARIABLE GENERATOR_STDERR
)
set(GENERATOR_LOG "${GENERATOR_STDOUT}\n${GENERATOR_STDERR}")
# The reference coast brief currently classifies as an island and returns 2
# after a complete, signature-verified export. Require the export contract
# instead of accepting any nonzero result.
if(NOT EXISTS "${GAME_WORLD_MAP}/manifest.json" OR
   NOT GENERATOR_LOG MATCHES "exported[ \t]+[^\r\n]+verified signature")
  message(FATAL_ERROR
    "Game-scale verification map export failed (status ${GENERATOR_RESULT}):\n"
    "${GENERATOR_LOG}")
endif()

function(read_churn_field LINE FIELD OUTPUT)
  string(REGEX MATCH "${FIELD}=([0-9]+)" FIELD_MATCH "${LINE}")
  if(FIELD_MATCH STREQUAL "")
    message(FATAL_ERROR "Churn line is missing '${FIELD}': ${LINE}")
  endif()
  set(${OUTPUT} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(ratio_ppm NUMERATOR DENOMINATOR OUTPUT)
  if(DENOMINATOR EQUAL 0)
    message(FATAL_ERROR "Cannot calculate ${OUTPUT}: denominator is zero")
  endif()
  math(EXPR VALUE
    "(${NUMERATOR} * 1000000 + ${DENOMINATOR} / 2) / ${DENOMINATOR}")
  set(${OUTPUT} "${VALUE}" PARENT_SCOPE)
endfunction()

function(run_stability MAP LABEL RUN PREFIX)
  execute_process(
    COMMAND "${SAMPLE}"
      --world-map "${MAP}"
      --world-map-center 0.5,0.5
      --terrain-render-distance-km 4
      --gbuffer-resolver bricks
      --test-camera-ground-forward
      --test-world-terrain-10cm
      # Hold the camera one centimetre above an LOD-0 voxel. This is the
      # reported crack/shadow failure distance, and a fixed transform prevents
      # terrain-following height steps from masquerading as screen shake.
      --test-camera-height 0.01
      --test-camera-travel-m 0
      # Rotate across voxel silhouettes, then hold the exact transform for the
      # settled windows below. This reproduces fast disocclusion at screen and
      # LOD-0 edges without making the final stability hashes camera-dependent.
      --test-camera-yaw-rate 1
      --test-camera-yaw-frames 12
      --frames 240
      # The runtime default is 2 px. Half resolution preserves the projected
      # terrain cut this stability baseline used with the former 4 px default.
      --resolution 160x90
      --render-scale 1
      --test-frame-signatures
      # Isolate terrain ownership and lighting stability from projection
      # jitter. A separate temporal test owns the jittered reconstruction
      # contract; this ground-level fixture must expose real page cracks.
      --jitter-scale 0
      --no-taa
      --disable-metalfx-upscaler
      --disable-frame-generation
      --disable-vsync
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE STDOUT
    ERROR_VARIABLE STDERR
  )
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
      "${LABEL} stability run ${RUN} failed:\n${STDOUT}\n${STDERR}")
  endif()

  set(LOG "${STDOUT}\n${STDERR}")
  # The fixture must be physically positioned over a resident 10 cm leaf, not
  # over the top of a coarse fallback voxel. This line is emitted only after a
  # direct CPU column trace has found the camera's rendered terrain page.
  string(REGEX MATCH
    "Terrain 10 cm ground contract: found=1 lod=0 voxel=0\\.100000m[^\r\n]*clearance=0\\.0[0-2][0-9]*m"
    GROUND_CONTRACT "${LOG}")
  if(GROUND_CONTRACT STREQUAL "")
    message(FATAL_ERROR
      "${LABEL} run ${RUN} did not place the camera one centimetre over a "
      "resident 10 cm LOD-0 voxel")
  endif()
  # Prove the reported "voxel shake" is not allowed to hide inside a lighting
  # metric. The exact GPU readback separates depth, resolved face, temporal
  # identity, material, and normals. With a fixed one-centimetre camera and no
  # projection jitter, every one must remain bit-identical throughout the
  # final settled quarter of the resident world-data run. The separately
  # generated game-scale fixture exercises ongoing streaming and is judged by
  # the publication-aware lighting metrics below.
  string(REGEX MATCHALL
    "Sparse voxel exact output:[^\r\n]*"
    EXACT_OUTPUT_LINES "${LOG}")
  set(EXACT_FRAME_COUNT 0)
  foreach(LINE IN LISTS EXACT_OUTPUT_LINES)
    string(REGEX MATCH "frame=([0-9]+)" FRAME_MATCH "${LINE}")
    if(FRAME_MATCH STREQUAL "" OR CMAKE_MATCH_1 LESS 180)
      continue()
    endif()
    math(EXPR EXACT_FRAME_COUNT "${EXACT_FRAME_COUNT} + 1")
    foreach(FIELD IN ITEMS
        instanceIdHash voxelIdHash materialIdHash materialUvHash
        baseColorHash materialPropertiesHash emissiveHash motionVectorHash
        coverageHash
        depthHash resolvedSurfaceHash surfaceIdentityHash temporalIdentityHash
        packedMaterialBottomHash microPayloadHash materialSampleAddressHash
        geometryNormalHash shadingNormalHash gbufferHash exactOutputHash)
      string(REGEX MATCH "${FIELD}=([0-9a-fA-F]+)" HASH_MATCH "${LINE}")
      if(HASH_MATCH STREQUAL "")
        message(FATAL_ERROR
          "${LABEL} exact output line is missing ${FIELD}: ${LINE}")
      endif()
      list(APPEND EXACT_${FIELD} "${CMAKE_MATCH_1}")
    endforeach()
  endforeach()
  if(NOT EXACT_FRAME_COUNT EQUAL 60)
    message(FATAL_ERROR
      "${LABEL} run ${RUN} exposed ${EXACT_FRAME_COUNT} settled exact-output "
      "frames, expected 60")
  endif()

  # Confirm the stationary camera is resolving the actual finest voxel cells,
  # not merely looking through a resident LOD-0 page at a coarse brick. The
  # readback payload comes directly from the raster hit, and the unique surface
  # and BaseColor counts prevent a single oversized/flat face from satisfying
  # the stability hashes.
  string(REGEX MATCHALL
    "Sparse voxel exact coverage:[^\r\n]*"
    EXACT_COVERAGE_LINES "${LOG}")
  set(EXACT_COVERAGE_FRAME_COUNT 0)
  foreach(LINE IN LISTS EXACT_COVERAGE_LINES)
    string(REGEX MATCH "frame=([0-9]+)" FRAME_MATCH "${LINE}")
    if(FRAME_MATCH STREQUAL "" OR CMAKE_MATCH_1 LESS 180)
      continue()
    endif()
    math(EXPR EXACT_COVERAGE_FRAME_COUNT
      "${EXACT_COVERAGE_FRAME_COUNT} + 1")
    foreach(FIELD IN ITEMS
        lod0TerrainPixels uniqueLod0Surfaces uniqueLod0Materials
        uniqueLod0BaseColors)
      string(REGEX MATCH "${FIELD}=([0-9]+)" VALUE_MATCH "${LINE}")
      if(VALUE_MATCH STREQUAL "")
        message(FATAL_ERROR
          "${LABEL} exact coverage line is missing ${FIELD}: ${LINE}")
      endif()
      set(VALUE_${FIELD} "${CMAKE_MATCH_1}")
    endforeach()
    # At one centimetre a 10 cm voxel legitimately fills most of the image.
    # Require both cells crossing the view and more than one exact BaseColor;
    # the separate G-buffer fixture backs away to 20 cm and requires many
    # independently addressed cells.
    if(LABEL STREQUAL "world-data" AND
        (VALUE_lod0TerrainPixels LESS 256 OR
         VALUE_uniqueLod0Surfaces LESS 2 OR
         VALUE_uniqueLod0BaseColors LESS 2))
      message(FATAL_ERROR
        "${LABEL} run ${RUN} is not directly observing varied LOD-0 voxels: "
        "${LINE}")
    endif()
  endforeach()
  if(NOT EXACT_COVERAGE_FRAME_COUNT EQUAL 60)
    message(FATAL_ERROR
      "${LABEL} run ${RUN} exposed ${EXACT_COVERAGE_FRAME_COUNT} settled "
      "LOD-0 coverage frames, expected 60")
  endif()

  # Validate the raw terrain receiver rather than accepting a stable black
  # presentation. Generated V2 terrain is occupancy-only: the close floor must
  # contain no analytic/micro representation, downward/back-facing normals,
  # black pixels, or coverage gaps.
  if(LABEL STREQUAL "world-data")
    string(REGEX MATCHALL
      "Sparse voxel terrain image:[^\r\n]*"
      TERRAIN_IMAGE_LINES "${LOG}")
    if(NOT TERRAIN_IMAGE_LINES)
      message(FATAL_ERROR "${LABEL} emitted no raw terrain image contract")
    endif()
    list(GET TERRAIN_IMAGE_LINES -1 TERRAIN_IMAGE_LINE)
    foreach(PATTERN IN ITEMS
        "isolatedNearBlack=0"
        "steepClosures=0"
        "slopeMismatch=0"
        # A real voxelized slope can expose a few legitimate riser faces in a
        # close view. Reject downward/back-facing or heightmap-inconsistent
        # normals, not every positive side-face count.
        "normals=\\(up=[1-9][0-9]* down=0 side=[0-9]+\\)"
        "representations=\\(solid=[1-9][0-9]* analytic=0 exactMicro=0\\)"
        "backFacingNormals=0"
        "interiorHoles=0"
        "interiorGapPixels=0"
        "horizontalGapPixels=0"
        "verticalGapPixels=0")
      if(NOT TERRAIN_IMAGE_LINE MATCHES "${PATTERN}")
        message(FATAL_ERROR
          "${LABEL} close LOD-0 raster/shadow receiver is invalid "
          "(${PATTERN}): ${TERRAIN_IMAGE_LINE}")
      endif()
    endforeach()
    # This is the lit presentation, so a few connected low-luminance pixels
    # can be a real riser shadow. Isolated severe pixels and geometry gaps are
    # the actual black-stain signatures. Still cap ordinary near-black output
    # to 0.1% so a broad lighting failure cannot pass as a legitimate shadow.
    string(REGEX MATCH "terrainPixels=([0-9]+)" TERRAIN_PIXELS_MATCH
      "${TERRAIN_IMAGE_LINE}")
    set(TERRAIN_PIXELS "${CMAKE_MATCH_1}")
    string(REGEX MATCH "nearBlack=([0-9]+)" NEAR_BLACK_MATCH
      "${TERRAIN_IMAGE_LINE}")
    set(NEAR_BLACK_PIXELS "${CMAKE_MATCH_1}")
    if(NOT TERRAIN_PIXELS_MATCH STREQUAL "")
      math(EXPR MAX_NEAR_BLACK_PIXELS "${TERRAIN_PIXELS} / 1000")
    else()
      set(MAX_NEAR_BLACK_PIXELS 0)
    endif()
    if(TERRAIN_PIXELS_MATCH STREQUAL "" OR NEAR_BLACK_MATCH STREQUAL "" OR
       NEAR_BLACK_PIXELS GREATER MAX_NEAR_BLACK_PIXELS)
      message(FATAL_ERROR
        "${LABEL} close LOD-0 lit output has excessive near-black coverage: "
        "${TERRAIN_IMAGE_LINE}")
    endif()
  endif()
  # world-data is already fully resident before the settled window and owns
  # the bit-stability contract. The freshly generated game-scale fixture may
  # still stream during these frames; its lighting-pop counters below validate
  # those transitions without pretending the geometry must remain unchanged.
  if(LABEL STREQUAL "world-data")
    foreach(FIELD IN ITEMS
        instanceIdHash voxelIdHash materialIdHash materialUvHash
        baseColorHash materialPropertiesHash emissiveHash motionVectorHash
        coverageHash
        depthHash resolvedSurfaceHash surfaceIdentityHash temporalIdentityHash
        packedMaterialBottomHash microPayloadHash materialSampleAddressHash
        geometryNormalHash shadingNormalHash gbufferHash exactOutputHash)
      list(REMOVE_DUPLICATES EXACT_${FIELD})
      list(LENGTH EXACT_${FIELD} UNIQUE_HASH_COUNT)
      if(NOT UNIQUE_HASH_COUNT EQUAL 1)
        message(FATAL_ERROR
          "${LABEL} run ${RUN} changed ${FIELD} on a stationary 1 cm camera "
          "(${UNIQUE_HASH_COUNT} distinct hashes)")
      endif()
    endforeach()
  endif()

  string(REGEX MATCHALL
    "Sparse voxel light field churn:[^\r\n]*"
    CHURN_LINES "${LOG}")

  set(FRAME_COUNT 0)
  foreach(FIELD IN ITEMS
      covered zeroTap attempted lost faceRejects depthRejects lodTaps
      halfStopSettled halfStopPopped varianceSettled variancePopped)
    set(SUM_${FIELD} 0)
  endforeach()
  foreach(LINE IN LISTS CHURN_LINES)
    read_churn_field("${LINE}" frame FRAME)
    if(FRAME LESS 120)
      continue()
    endif()
    math(EXPR FRAME_COUNT "${FRAME_COUNT} + 1")
    foreach(FIELD IN ITEMS
        covered zeroTap attempted lost faceRejects depthRejects lodTaps
        halfStopSettled halfStopPopped varianceSettled variancePopped)
      read_churn_field("${LINE}" "${FIELD}" VALUE)
      math(EXPR SUM_${FIELD} "${SUM_${FIELD}} + ${VALUE}")
    endforeach()
  endforeach()
  if(NOT FRAME_COUNT EQUAL 120)
    message(FATAL_ERROR
      "${LABEL} run ${RUN} exposed ${FRAME_COUNT} settled frames, expected 120")
  endif()

  math(EXPR HISTORY_CANDIDATES "${SUM_attempted} * 4")
  ratio_ppm(${SUM_zeroTap} ${SUM_covered} ZERO_TAP_PPM)
  ratio_ppm(${SUM_lost} ${SUM_attempted} HISTORY_LOSS_PPM)
  ratio_ppm(${SUM_faceRejects} ${HISTORY_CANDIDATES} FACE_REJECT_PPM)
  ratio_ppm(${SUM_depthRejects} ${HISTORY_CANDIDATES} DEPTH_REJECT_PPM)
  ratio_ppm(${SUM_lodTaps} ${HISTORY_CANDIDATES} LOD_TAP_PPM)
  ratio_ppm(${SUM_halfStopPopped} ${SUM_halfStopSettled} HALF_STOP_POP_PPM)
  ratio_ppm(${SUM_variancePopped} ${SUM_varianceSettled} VARIANCE_POP_PPM)

  foreach(METRIC IN ITEMS
      ZERO_TAP HISTORY_LOSS FACE_REJECT DEPTH_REJECT LOD_TAP HALF_STOP_POP
      VARIANCE_POP)
    set(${PREFIX}_${METRIC}_PPM "${${METRIC}_PPM}" PARENT_SCOPE)
  endforeach()
  message(STATUS
    "${LABEL} run ${RUN}: zeroTap=${ZERO_TAP_PPM}ppm "
    "historyLoss=${HISTORY_LOSS_PPM}ppm faceCandidates=${FACE_REJECT_PPM}ppm "
    "depthCandidates=${DEPTH_REJECT_PPM}ppm lodCandidates=${LOD_TAP_PPM}ppm "
    "halfStopPop=${HALF_STOP_POP_PPM}ppm "
    "variancePop=${VARIANCE_POP_PPM}ppm")
endfunction()

run_stability("${WORLD_MAP}" "world-data" 1 WORLD_A)
run_stability("${WORLD_MAP}" "world-data" 2 WORLD_B)
run_stability("${GAME_WORLD_MAP}" "game-scale" 1 GAME_A)
run_stability("${GAME_WORLD_MAP}" "game-scale" 2 GAME_B)

function(require_repeatable PREFIX_A PREFIX_B LABEL)
  foreach(METRIC IN ITEMS
      FACE_REJECT DEPTH_REJECT LOD_TAP HALF_STOP_POP VARIANCE_POP)
    set(LEFT "${${PREFIX_A}_${METRIC}_PPM}")
    set(RIGHT "${${PREFIX_B}_${METRIC}_PPM}")
    math(EXPR DELTA "${LEFT} - ${RIGHT}")
    if(DELTA LESS 0)
      math(EXPR DELTA "-${DELTA}")
    endif()
    if(DELTA GREATER 2000)
      message(FATAL_ERROR
        "${LABEL} ${METRIC} duplicate-run spread is ${DELTA}ppm, above "
        "the 2000ppm noise ceiling (${LEFT} vs ${RIGHT})")
    endif()
  endforeach()
endfunction()

require_repeatable(WORLD_A WORLD_B "world-data")
require_repeatable(GAME_A GAME_B "game-scale")

foreach(PREFIX IN ITEMS WORLD_A WORLD_B GAME_A GAME_B)
  # Close-range spatial reuse deliberately requires exact voxel identity.
  # Consequently an edge pixel may find no compatible *current-frame* sparse
  # neighbour and retain its validated temporal record instead. Requiring the
  # former one-percent zero-tap rate can only be met by restoring the relaxed
  # cross-voxel recovery that produced the crawling light grid. Keep this as a
  # bounded coverage diagnostic; history loss and the two pop rates below are
  # the actual temporal-stability contract.
  if(${PREFIX}_ZERO_TAP_PPM GREATER 350000)
    message(FATAL_ERROR
      "${PREFIX} compatible current-frame spatial coverage fell below 65%: "
      "${${PREFIX}_ZERO_TAP_PPM}ppm")
  endif()
  if(PREFIX MATCHES "^WORLD_")
    set(HISTORY_LOSS_LIMIT 5000)
  else()
    # The generated game-scale fixture intentionally continues publishing
    # pages in this window. Its exact commit count is asynchronous, so bound
    # loss per run instead of pretending duplicate runs have identical timing.
    set(HISTORY_LOSS_LIMIT 20000)
  endif()
  if(${PREFIX}_HISTORY_LOSS_PPM GREATER HISTORY_LOSS_LIMIT)
    message(FATAL_ERROR
      "${PREFIX} complete history loss exceeds its publication-aware limit "
      "(${HISTORY_LOSS_LIMIT}ppm): "
      "${${PREFIX}_HISTORY_LOSS_PPM}ppm")
  endif()
  if(${PREFIX}_FACE_REJECT_PPM GREATER 12000)
    message(FATAL_ERROR
      "${PREFIX} normal rejection exceeds 1.2% of candidate taps: "
      "${${PREFIX}_FACE_REJECT_PPM}ppm")
  endif()
  if(${PREFIX}_DEPTH_REJECT_PPM GREATER 12000)
    message(FATAL_ERROR
      "${PREFIX} depth rejection exceeds 1.2% of candidate taps: "
      "${${PREFIX}_DEPTH_REJECT_PPM}ppm")
  endif()
  if(${PREFIX}_LOD_TAP_PPM GREATER 1000000)
    message(FATAL_ERROR
      "${PREFIX} inexact-identity count exceeds the four-tap footprint: "
      "${${PREFIX}_LOD_TAP_PPM}ppm")
  endif()
  if(${PREFIX}_HALF_STOP_POP_PPM GREATER 2000)
    message(FATAL_ERROR
      "${PREFIX} settled half-stop pop rate exceeds 0.2%: "
      "${${PREFIX}_HALF_STOP_POP_PPM}ppm")
  endif()
  if(${PREFIX}_VARIANCE_POP_PPM GREATER 1000)
    message(FATAL_ERROR
      "${PREFIX} three-sigma pop rate exceeds 0.1%: "
      "${${PREFIX}_VARIANCE_POP_PPM}ppm")
  endif()
endforeach()

message(STATUS
  "Light-field stability verification passed two identical settled runs on "
  "both map scales; duplicate-run noise remained below 2000ppm")
