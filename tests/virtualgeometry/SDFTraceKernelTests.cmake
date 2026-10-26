cmake_minimum_required(VERSION 3.10)

project(SDFTraceKernelTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SDFTraceKernelTests ${TEST_DIR}/SDFTraceKernelTests.cpp)
target_link_libraries(SDFTraceKernelTests PRIVATE Engine)
add_dependencies(SDFTraceKernelTests SDFShaders copy_mesh_assets)
add_test(NAME SDFTraceKernelTests COMMAND SDFTraceKernelTests)
