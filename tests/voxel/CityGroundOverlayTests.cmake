add_executable(CityGroundOverlayTests
    ${CMAKE_CURRENT_LIST_DIR}/CityGroundOverlayTests.cpp)
target_link_libraries(CityGroundOverlayTests PRIVATE Engine)
add_test(NAME CityGroundOverlayTests COMMAND CityGroundOverlayTests)
set_tests_properties(CityGroundOverlayTests PROPERTIES LABELS "svraster.cityvoxel.ground")
