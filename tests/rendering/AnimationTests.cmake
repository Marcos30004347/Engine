set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rendering)

add_executable(AnimationTests ${TEST_DIR}/AnimationTests.cpp)
target_link_libraries(AnimationTests PRIVATE Engine)

add_test(NAME AnimationTests COMMAND AnimationTests)
