add_executable(WorldGIDDGIGridTests ${TEST_DIR}/WorldGIDDGIGridTests.cpp)
target_link_libraries(WorldGIDDGIGridTests PRIVATE Engine)
add_test(NAME WorldGIDDGIGridTests COMMAND WorldGIDDGIGridTests)
set_tests_properties(WorldGIDDGIGridTests PROPERTIES LABELS "worldgi.ddgi;cpu")
