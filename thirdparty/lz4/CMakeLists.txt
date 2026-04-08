cmake_minimum_required(VERSION 3.16)
project(lz4 C)

add_library(lz4 STATIC lib/lz4.c)

target_include_directories(lz4 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lib)

if (MSVC)
    target_compile_options(lz4 PRIVATE /W0)
else()
    target_compile_options(lz4 PRIVATE -w)
endif()

set_target_properties(lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
