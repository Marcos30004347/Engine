file(GLOB_RECURSE ENGINE_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/runtime/*.c"
    "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/rpmalloc/*.c"
    "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap/*.c"
)

add_library(runtime ${ENGINE_RUNTIME_SOURCES})
engine_apply_runtime_target_defaults(runtime)
