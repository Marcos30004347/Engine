if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Read back the actual attachment-quantized output of the GPU brick resolver
# close enough to inspect individual LOD-0 cells while far enough for several
# 10 cm voxels to enter the 65-degree-down frustum. The separate light-field
# fixture owns the exact one-centimetre stability contract.
execute_process(
  COMMAND "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --world-map-center 0.5,0.5
    --terrain-render-distance-km 4
    --gbuffer-resolver bricks
    --test-terrain-gbuffer
    --test-camera-height 0.20
    --test-camera-travel-m 0
    # The full-GPU default starts from the coarse four-page safety cut and
    # refines one complete ownership group at a time.  Give that chain time to
    # reach the actual 10 cm owner; sampling at frame 180 only tested the
    # unresolved bootstrap and could report zero LOD-0 pixels.
    --frames 600
    --resolution 160x90
    --render-scale 1
    --jitter-scale 0
    --disable-lighting
    --no-taa
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)
set(LOG "${STDOUT}\n${STDERR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Terrain G-buffer run failed:\n${LOG}")
endif()

# The semantic validator runs directly over the GPU readback.  Check every
# settled frame, rather than accepting a final frame that happened to recover.
string(REGEX MATCHALL
  "Sparse voxel terrain G-buffer contract:[^\r\n]*"
  CONTRACT_LINES "${LOG}")
set(CONTRACT_FRAME_COUNT 0)
foreach(LINE IN LISTS CONTRACT_LINES)
  string(REGEX MATCH "frame=([0-9]+)" FRAME_MATCH "${LINE}")
  if(FRAME_MATCH STREQUAL "" OR CMAKE_MATCH_1 LESS 540)
    continue()
  endif()
  math(EXPR CONTRACT_FRAME_COUNT "${CONTRACT_FRAME_COUNT} + 1")
  foreach(PATTERN IN ITEMS
      "valid=1"
      "terrainPixels=[1-9][0-9][0-9][0-9]*"
      "errors=0"
      "coverageHitMismatch=0"
      "invalidSurface=0"
      "invalidFace=0"
      "instanceIdMismatch=0"
      "voxelIdMismatch=0"
      "materialIdMismatch=0"
      "invalidMaterialUv=0"
      "invalidBaseColor=0"
      "invalidRoughness=0"
      "invalidMetallic=0"
      "invalidLayerWeight=0"
      "materialContextMismatch=0"
      "invalidEmissive=0"
      "invalidGeometryNormal=0"
      "geometryFaceMismatch=0"
      "invalidShadingNormal=0"
      "shadingNormalMismatch=0"
      "invalidMotion=0"
      "invalidDepth=0"
      "temporalIdMismatch=0"
      "nonUniformVoxelFaceAlbedo=0")
    if(NOT LINE MATCHES "${PATTERN}")
      message(FATAL_ERROR
        "Settled terrain G-buffer violates '${PATTERN}': ${LINE}")
    endif()
  endforeach()
  foreach(FIELD IN ITEMS
      uniqueVoxels uniqueMaterialAddresses uniqueBaseColors
      uniqueRoughnessValues)
    string(REGEX MATCH "${FIELD}=([0-9]+)" VALUE_MATCH "${LINE}")
    if(VALUE_MATCH STREQUAL "")
      message(FATAL_ERROR "G-buffer contract is missing ${FIELD}: ${LINE}")
    endif()
    set(VALUE_${FIELD} "${CMAKE_MATCH_1}")
  endforeach()
  if(VALUE_uniqueVoxels LESS 4 OR
     VALUE_uniqueMaterialAddresses LESS 4 OR
     VALUE_uniqueBaseColors LESS 2 OR
     VALUE_uniqueRoughnessValues LESS 1)
    message(FATAL_ERROR
      "Settled terrain G-buffer lacks independently addressed/materialized "
      "LOD-0 voxels: ${LINE}")
  endif()
endforeach()
if(NOT CONTRACT_FRAME_COUNT EQUAL 60)
  message(FATAL_ERROR
    "Expected 60 settled G-buffer contracts, found ${CONTRACT_FRAME_COUNT}")
endif()

# A semantic range check cannot detect a valid value attached to the wrong
# voxel on alternating frames. Require every identity and every material
# attachment to remain bit-identical for the stationary close camera.
string(REGEX MATCHALL
  "Sparse voxel exact output:[^\r\n]*"
  EXACT_LINES "${LOG}")
set(EXACT_FRAME_COUNT 0)
foreach(LINE IN LISTS EXACT_LINES)
  string(REGEX MATCH "frame=([0-9]+)" FRAME_MATCH "${LINE}")
  if(FRAME_MATCH STREQUAL "" OR CMAKE_MATCH_1 LESS 540)
    continue()
  endif()
  math(EXPR EXACT_FRAME_COUNT "${EXACT_FRAME_COUNT} + 1")
  foreach(FIELD IN ITEMS
      instanceIdHash voxelIdHash materialIdHash materialUvHash
      baseColorHash materialPropertiesHash emissiveHash motionVectorHash
      coverageHash depthHash resolvedSurfaceHash surfaceIdentityHash
      temporalIdentityHash packedMaterialBottomHash microPayloadHash
      materialSampleAddressHash geometryNormalHash shadingNormalHash
      gbufferHash exactOutputHash)
    string(REGEX MATCH "${FIELD}=([0-9a-fA-F]+)" VALUE_MATCH "${LINE}")
    if(VALUE_MATCH STREQUAL "")
      message(FATAL_ERROR "Exact output is missing ${FIELD}: ${LINE}")
    endif()
    list(APPEND VALUES_${FIELD} "${CMAKE_MATCH_1}")
  endforeach()
endforeach()
if(NOT EXACT_FRAME_COUNT EQUAL 60)
  message(FATAL_ERROR
    "Expected 60 settled exact outputs, found ${EXACT_FRAME_COUNT}")
endif()
foreach(FIELD IN ITEMS
    instanceIdHash voxelIdHash materialIdHash materialUvHash
    baseColorHash materialPropertiesHash emissiveHash motionVectorHash
    coverageHash depthHash resolvedSurfaceHash surfaceIdentityHash
    temporalIdentityHash packedMaterialBottomHash microPayloadHash
    materialSampleAddressHash geometryNormalHash shadingNormalHash
    gbufferHash exactOutputHash)
  list(REMOVE_DUPLICATES VALUES_${FIELD})
  list(LENGTH VALUES_${FIELD} UNIQUE_COUNT)
  if(NOT UNIQUE_COUNT EQUAL 1)
    message(FATAL_ERROR
      "Stationary close terrain changed ${FIELD} across settled frames "
      "(${UNIQUE_COUNT} distinct values)")
  endif()
endforeach()

message(STATUS
  "Terrain G-buffer contract passed: all LOD-0 identities, material channels, "
  "normals, motion, depth, and temporal IDs were valid and stable")
