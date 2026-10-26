cmake_minimum_required(VERSION 3.10)

project(MaterialPhase1Tests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(MaterialPhase1Tests ${TEST_DIR}/MaterialPhase1Tests.cpp)
target_link_libraries(MaterialPhase1Tests PRIVATE Engine)
add_dependencies(MaterialPhase1Tests wgsl_material_pass_compute_shader wgsl_default_material_compute_shader)
add_test(NAME MaterialPhase1Tests COMMAND MaterialPhase1Tests)
