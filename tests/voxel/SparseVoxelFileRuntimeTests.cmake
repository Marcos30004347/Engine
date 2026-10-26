cmake_minimum_required(VERSION 3.10)

project(SparseVoxelFileRuntimeTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelFileRuntimeTests ${TEST_DIR}/SparseVoxelFileRuntimeTests.cpp)
target_link_libraries(SparseVoxelFileRuntimeTests PRIVATE Engine)

add_test(NAME SparseVoxelFileRuntimeG18Tests COMMAND SparseVoxelFileRuntimeTests)
set_tests_properties(SparseVoxelFileRuntimeG18Tests PROPERTIES LABELS "svraster.g18;svraster.m2")
