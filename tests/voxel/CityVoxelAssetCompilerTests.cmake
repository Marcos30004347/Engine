add_executable(CityVoxelAssetCompilerTests
    ${CMAKE_CURRENT_LIST_DIR}/CityVoxelAssetCompilerTests.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelAssetCompiler.cpp
    ${CMAKE_SOURCE_DIR}/src/voxel/svraster/CityVoxelExactAppearanceLookup.cpp)
target_include_directories(CityVoxelAssetCompilerTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(CityVoxelAssetCompilerTests PRIVATE cxx_std_17)
add_test(NAME CityVoxelAssetCompilerTests COMMAND CityVoxelAssetCompilerTests)
set_tests_properties(CityVoxelAssetCompilerTests PROPERTIES LABELS "svraster.cityvoxel.phase4")
