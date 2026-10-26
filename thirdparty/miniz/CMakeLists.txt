cmake_minimum_required(VERSION 3.16)
project(miniz C)

add_library(miniz STATIC miniz.c)

target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_LIST_DIR})

if (MSVC)
    target_compile_options(miniz PRIVATE /W0)
else()
    target_compile_options(miniz PRIVATE -w)
endif()

set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
