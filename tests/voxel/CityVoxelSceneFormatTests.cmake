add_executable(CityVoxelSceneFormatTests
    ${CMAKE_CURRENT_LIST_DIR}/CityVoxelSceneFormatTests.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelSceneFormat.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelSceneFile.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelPageCompression.cpp)
target_include_directories(CityVoxelSceneFormatTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(CityVoxelSceneFormatTests PRIVATE cxx_std_17)
target_link_libraries(CityVoxelSceneFormatTests PRIVATE lz4)
add_executable(CityVoxelSceneFileProbe
    ${CMAKE_CURRENT_LIST_DIR}/CityVoxelSceneFileProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelSceneFormat.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelSceneFile.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelPageCompression.cpp)
target_include_directories(CityVoxelSceneFileProbe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(CityVoxelSceneFileProbe PRIVATE cxx_std_17)
target_link_libraries(CityVoxelSceneFileProbe PRIVATE lz4)
target_compile_definitions(CityVoxelSceneFormatTests PRIVATE
    CITY_VOXEL_SCENE_PROBE="$<TARGET_FILE:CityVoxelSceneFileProbe>")
add_dependencies(CityVoxelSceneFormatTests CityVoxelSceneFileProbe)
add_test(NAME CityVoxelSceneFormatTests COMMAND CityVoxelSceneFormatTests)
set_tests_properties(CityVoxelSceneFormatTests PROPERTIES LABELS "svraster.cityvoxel.phase1")
