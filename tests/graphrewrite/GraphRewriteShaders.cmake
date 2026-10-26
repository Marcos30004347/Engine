cmake_minimum_required(VERSION 3.10)

# Compiling these through naga is itself a test: it is what catches the WGSL
# constructs this engine's toolchain rejects, such as a loop that can only be
# left by returning.


wgsl2spirv(
    NAME graphrewrite_publish_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/graphrewrite/wgsl/graph-publish.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/graphrewrite-publish.spirv
)


wgsl2spirv(
    NAME graphrewrite_rewrite_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/graphrewrite/wgsl/graph-rewrite.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/graphrewrite-rewrite.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/graphrewrite/wgsl
)

# One module per edges-per-thread setting, so the benchmark can compare them
# without rebuilding.
foreach(EPT 1 8 16 32)
    wgsl2spirv(
        NAME graphrewrite_match_cs_ept${EPT}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/graphrewrite/wgsl/graph-match.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/graphrewrite-match-ept${EPT}.spirv
        DEFINES EDGES_PER_THREAD=${EPT}
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/graphrewrite/wgsl
    )
endforeach()

add_custom_target(graphrewrite_match_cs)
add_dependencies(graphrewrite_match_cs
    graphrewrite_match_cs_ept1
    graphrewrite_match_cs_ept8
    graphrewrite_match_cs_ept16
    graphrewrite_match_cs_ept32)
