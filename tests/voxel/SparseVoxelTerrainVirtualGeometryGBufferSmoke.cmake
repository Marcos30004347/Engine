if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

execute_process(
  COMMAND "${SAMPLE}"
    --disable-lighting --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 5
    --terrain-builder gpu-full
    --terrain-gpu-memory-mb 8192
    --frames 240
    --test-frame-signatures
    --test-capture-start-frame 216
    # Use the interactive terrain error budget, allowing streaming to refine
    # before checking coverage. The old implicit 64 px/32-frame fixture accepted
    # coarse fallback walls. 360p also fits the full GPU route on a 16 GiB Mac.
    --resolution 640x360
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
    "Terrain virtual-geometry G-buffer smoke failed:\n${LOG}")
endif()
# The route must actually generate pages on the GPU, not just register passes.
string(REGEX MATCHALL "Terrain generated-VG page resident:" SMOKE_INSTALLS "${LOG}")
list(LENGTH SMOKE_INSTALLS SMOKE_INSTALL_COUNT)
if(SMOKE_INSTALL_COUNT LESS 4)
  message(FATAL_ERROR
    "Terrain virtual-geometry G-buffer smoke installed no generated pages "
    "(${SMOKE_INSTALL_COUNT} in 240 frames):\n${LOG}")
endif()

foreach(PASS_NAME
    "svr.sample.TerrainVGRenderer.pageDecode"
    "svr.sample.TerrainVGRenderer.preCulling"
    "svr.sample.TerrainVGRenderer.depthPyramid"
    "svr.sample.TerrainVGRenderer.finalCulling"
    "svr.sample.TerrainVGRenderer.streamingSelection"
    "svr.sample.TerrainVGRenderer.hardwareDraw"
    "svr.sample.TerrainVGLinearDepthAdapter")
  string(FIND "${LOG}" "Registered pass ${PASS_NAME}" PASS_OFFSET)
  if(PASS_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "Terrain VG smoke did not register required pass ${PASS_NAME}")
  endif()
endforeach()

# The depth prepass and the two-pass occlusion phase-one draw are alternative
# first halves of the same chain, selected by --vg-two-phase-occlusion. Require
# one of them rather than pinning the choice.
string(FIND "${LOG}" "Registered pass svr.sample.TerrainVGRenderer.depthPrePass"
    DEPTH_PREPASS_OFFSET)
string(FIND "${LOG}" "Registered pass svr.sample.TerrainVGRenderer.phaseOneDraw"
    PHASE_ONE_OFFSET)
if(DEPTH_PREPASS_OFFSET EQUAL -1 AND PHASE_ONE_OFFSET EQUAL -1)
  message(FATAL_ERROR
    "Terrain VG smoke registered neither depthPrePass nor phaseOneDraw")
endif()

string(FIND "${LOG}" "Terrain G-buffer resolver request=terrain-vg selected=terrain virtual geometry route=virtual-geometry" ROUTE_OFFSET)
if(ROUTE_OFFSET EQUAL -1)
  message(FATAL_ERROR
    "Terrain VG smoke did not use the default terrain-vg resolver:\n${LOG}")
endif()

# Two mutually exclusive chains, selected by --vg-two-phase-occlusion:
#
#   two-pass    - phase one draws last frame's cluster set into the real target
#                 at full resolution, phase two draws the rest. There is no
#                 reduced-resolution prepass to assert.
#   depth prepass - the older chain, which rasterizes every cluster twice and
#                 runs the first pass at prepassResolutionDivisor.
string(REGEX MATCH
  "Virtual Geometry two-pass occlusion: pass=svr\\.sample\\.TerrainVGRenderer full=([0-9]+)x([0-9]+) hzbMips=([0-9]+)"
  TWO_PASS_MATCH "${LOG}")
if(NOT TWO_PASS_MATCH STREQUAL "")
  if(CMAKE_MATCH_3 LESS 1)
    message(FATAL_ERROR
      "VirtualGeometry two-pass occlusion published no HZB mips: ${TWO_PASS_MATCH}")
  endif()
  string(FIND "${LOG}"
      "Registered pass svr.sample.TerrainVGRenderer.phaseOneDraw"
      TWO_PASS_PHASE_ONE_OFFSET)
  if(TWO_PASS_PHASE_ONE_OFFSET EQUAL -1)
    message(FATAL_ERROR
      "VirtualGeometry two-pass occlusion did not register its phase-one draw")
  endif()
else()

string(REGEX MATCH
  "Virtual Geometry low-resolution prepass: pass=svr\\.sample\\.TerrainVGRenderer full=([0-9]+)x([0-9]+) prepass=([0-9]+)x([0-9]+) divisor=([0-9]+) hzbMips=([0-9]+)"
  PREPASS_MATCH "${LOG}")
if(PREPASS_MATCH STREQUAL "")
  message(FATAL_ERROR
    "Terrain VG smoke emitted no generic VirtualGeometry low-resolution "
    "prepass contract")
endif()
set(FULL_WIDTH "${CMAKE_MATCH_1}")
set(FULL_HEIGHT "${CMAKE_MATCH_2}")
set(PREPASS_WIDTH "${CMAKE_MATCH_3}")
set(PREPASS_HEIGHT "${CMAKE_MATCH_4}")
set(PREPASS_DIVISOR "${CMAKE_MATCH_5}")
set(PREPASS_HZB_MIPS "${CMAKE_MATCH_6}")
if(NOT PREPASS_WIDTH LESS FULL_WIDTH OR NOT PREPASS_HEIGHT LESS FULL_HEIGHT)
  message(FATAL_ERROR
    "VirtualGeometry prepass was not lower resolution: ${PREPASS_MATCH}")
endif()
if(PREPASS_DIVISOR LESS 2 OR PREPASS_HZB_MIPS LESS 1)
  message(FATAL_ERROR
    "VirtualGeometry prepass contract is not useful: ${PREPASS_MATCH}")
endif()

endif()

string(REGEX MATCHALL
  "Terrain generated-VG page resident:[^\r\n]*vg\\(faces=([0-9]+) clusters=([0-9]+) vertices=([0-9]+) indices=([0-9]+)"
  GENERATED_VG_LINES "${LOG}")
if(NOT GENERATED_VG_LINES)
  message(FATAL_ERROR
    "Terrain VG smoke did not publish GPU-generated decoded VG pages")
endif()

string(REGEX MATCHALL
  "Sparse voxel smoke test passed:[^\r\n]*"
  COVERAGE_LINES "${LOG}")
if(NOT COVERAGE_LINES)
  message(FATAL_ERROR "Terrain VG smoke emitted no renderer smoke contract")
endif()
list(GET COVERAGE_LINES -1 COVERAGE_LINE)
string(REGEX MATCH
  "frames=([0-9]+).*visibleBricks=([0-9]+).*selectedLegacyLODNodes=([0-9]+).*visibleVoxels=([0-9]+).*coveredPixels=([0-9]+)"
  COVERAGE_MATCH "${COVERAGE_LINE}")
if(COVERAGE_MATCH STREQUAL "")
  message(FATAL_ERROR "Could not parse Terrain VG renderer smoke: ${COVERAGE_LINE}")
endif()
set(SMOKE_FRAMES "${CMAKE_MATCH_1}")
set(VISIBLE_BRICKS "${CMAKE_MATCH_2}")
set(LEGACY_LOD_NODES "${CMAKE_MATCH_3}")
set(VISIBLE_VOXELS "${CMAKE_MATCH_4}")
set(COVERED_PIXELS "${CMAKE_MATCH_5}")

if(SMOKE_FRAMES LESS 240)
  message(FATAL_ERROR
    "Terrain VG smoke did not run long enough for GPU-generated pages to "
    "settle: ${COVERAGE_LINE}")
endif()
if(COVERED_PIXELS LESS 100000)
  message(FATAL_ERROR
    "Terrain VG renderer coverage is too low: ${COVERAGE_LINE}")
endif()
if(NOT VISIBLE_BRICKS EQUAL 0 OR
   NOT LEGACY_LOD_NODES EQUAL 0 OR
   NOT VISIBLE_VOXELS EQUAL 0)
  message(FATAL_ERROR
    "Terrain VG smoke used a legacy sparse-voxel/brick raster path: "
    "${COVERAGE_LINE}")
endif()

# Installation counters alone do not prove that decoded geometry survives
# later asynchronous uploads. Check every settled frame, on both frame slots.
string(REGEX MATCHALL
  "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+"
  FRAME_SIGNATURES "${LOG}")
set(SETTLED_FRAMES 0)
foreach(SIGNATURE IN LISTS FRAME_SIGNATURES)
  if(SIGNATURE MATCHES "frame=([0-9]+) coveredPixels=([0-9]+)")
    if(CMAKE_MATCH_1 GREATER_EQUAL 216)
      if(CMAKE_MATCH_2 LESS 100000)
        message(FATAL_ERROR "Asynchronous terrain lost geometry: ${SIGNATURE}")
      endif()
      math(EXPR SETTLED_FRAMES "${SETTLED_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(SETTLED_FRAMES LESS 24)
  message(FATAL_ERROR "Terrain smoke needs 24 settled frame signatures")
endif()

message(STATUS
  "Terrain VG G-buffer smoke passed: frames=${SMOKE_FRAMES}, "
  "coverage=${COVERED_PIXELS}, generatedPages=${SMOKE_INSTALL_COUNT}")
