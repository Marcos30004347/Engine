add_executable(FacadeAtlasTests ${CMAKE_CURRENT_LIST_DIR}/FacadeAtlasTests.cpp)
target_link_libraries(FacadeAtlasTests PRIVATE Engine)
add_test(NAME FacadeAtlasTests COMMAND FacadeAtlasTests)
set_tests_properties(FacadeAtlasTests PROPERTIES LABELS "dwellings;structure;mesh")
