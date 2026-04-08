cmake_minimum_required(VERSION 3.10)

project (VirtualGeometryHardwareDrawPassTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

set(VG_FRAME_STAT_SHADER_DEFINES)
if(ENGINE_COLLECT_FRAME_STATISTICS)
    list(APPEND VG_FRAME_STAT_SHADER_DEFINES COLLECT_FRAME_STATISTICS)
endif()


wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs-draw_indirect_count_disabled.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING DRAW_INDIRECT_COUNT_DISABLED
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-fs.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING ${VG_FRAME_STAT_SHADER_DEFINES}
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_material_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-fs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_material_vertex_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-vs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_material_depth_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-depth-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-depth-fs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_material_tiles_compute_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-tiles-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-tiles-cs.spirv
)

wgsl2spirv(
    NAME wgsl_renderToQuad_vertex_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadPass-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadPass-vs.spirv
    DEFINES USE_REVERSE_Z
)
wgsl2spirv(
    NAME wgsl_renderToQuad_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadPass-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadPass-fs.spirv
    DEFINES USE_REVERSE_Z
)
wgsl2spirv(
    NAME wgsl_renderToQuadColor_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderColorToQuadPass-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderColorToQuadPass-fs.spirv
    DEFINES USE_REVERSE_Z
)

add_executable(VirtualGeometryHardwareDrawPassTests ${TEST_DIR}/VirtualGeometryHardwareDrawPassTests.cpp)
target_link_libraries(VirtualGeometryHardwareDrawPassTests PRIVATE Engine)
add_test(NAME VirtualGeometryHardwareDrawPassTests COMMAND VirtualGeometryHardwareDrawPassTests)

add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_vertex_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_fragment_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_material_fragment_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_material_vertex_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_material_depth_fragment_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_virtualgeometry_material_tiles_compute_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_renderToQuad_fragment_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_renderToQuad_vertex_shader)
add_dependencies(VirtualGeometryHardwareDrawPassTests wgsl_renderToQuadColor_fragment_shader)
