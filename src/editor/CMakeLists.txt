include(${CMAKE_SOURCE_DIR}/cmake/meshoptimizer.cmake)
include(FetchContent)

FetchContent_Declare(
    metis
    GIT_REPOSITORY https://github.com/Marcos30004347/METIS.git
    GIT_TAG        main
    SOURCE_DIR     ${CMAKE_SOURCE_DIR}/thirdparty/METIS
    BINARY_DIR     ${CMAKE_BINARY_DIR}/thirdparty/METIS-build
)

FetchContent_MakeAvailable(metis)

add_compile_definitions(
    IDXTYPEWIDTH=32
    REALTYPEWIDTH=64
)

file(GLOB_RECURSE ENGINE_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/editor/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/editor/*.c"
)

add_library(editor ${ENGINE_EDITOR_SOURCES})
engine_apply_common_includes(editor)
target_include_directories(editor PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime/virtualgeometry)
target_link_libraries(editor
    PUBLIC runtime
    PRIVATE lz4
    PRIVATE miniz
    PRIVATE metis
    PRIVATE meshoptimizer
)
