# Standalone CPU experiment: no Engine, shader compiler, or RT device required.
get_filename_component(VG_SHADOW_TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(VirtualGeometryShadowCutReferenceTests
    ${VG_SHADOW_TEST_DIR}/VirtualGeometryShadowCutReferenceTests.cpp)
target_include_directories(VirtualGeometryShadowCutReferenceTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
target_compile_features(VirtualGeometryShadowCutReferenceTests PRIVATE cxx_std_17)
add_test(NAME VirtualGeometryShadowCutReferenceTests COMMAND VirtualGeometryShadowCutReferenceTests)
set_tests_properties(VirtualGeometryShadowCutReferenceTests PROPERTIES LABELS "virtualgeometry;shadow;cpu")
