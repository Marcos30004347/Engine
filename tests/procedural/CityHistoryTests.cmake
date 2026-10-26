add_executable(CityHistoryTests ${CMAKE_CURRENT_LIST_DIR}/CityHistoryTests.cpp)
target_link_libraries(CityHistoryTests PRIVATE runtime)
target_include_directories(CityHistoryTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityHistoryTests COMMAND CityHistoryTests)
set_tests_properties(CityHistoryTests PROPERTIES TIMEOUT 60 LABELS "city;history")
add_executable(CityHistoryPreview ${CMAKE_SOURCE_DIR}/tools/terrain/CityHistoryPreview.cpp)
target_link_libraries(CityHistoryPreview PRIVATE Engine)
target_include_directories(CityHistoryPreview PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME CityHistoryIntegration COMMAND ${CMAKE_COMMAND}
    -DPREVIEW=$<TARGET_FILE:CityHistoryPreview>
    -DASSETS=${CMAKE_SOURCE_DIR}/assets/graphrewrite
    -DOUTPUT=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_LIST_DIR}/CityHistoryIntegration.cmake)
set_tests_properties(CityHistoryIntegration PROPERTIES TIMEOUT 620 LABELS "city;history;integration")
add_executable(CityHouseholdTests ${CMAKE_CURRENT_LIST_DIR}/CityHouseholdTests.cpp)
target_link_libraries(CityHouseholdTests PRIVATE runtime)
target_include_directories(CityHouseholdTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityHouseholdTests COMMAND CityHouseholdTests)
set_tests_properties(CityHouseholdTests PROPERTIES TIMEOUT 60 LABELS "city;history")
add_executable(CityEconomyTests ${CMAKE_CURRENT_LIST_DIR}/CityEconomyTests.cpp)
target_link_libraries(CityEconomyTests PRIVATE runtime)
target_include_directories(CityEconomyTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityEconomyTests COMMAND CityEconomyTests)
set_tests_properties(CityEconomyTests PROPERTIES TIMEOUT 60 LABELS "city;history")
add_executable(CityRealmTests ${CMAKE_CURRENT_LIST_DIR}/CityRealmTests.cpp)
target_link_libraries(CityRealmTests PRIVATE runtime)
target_include_directories(CityRealmTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityRealmTests COMMAND CityRealmTests)
set_tests_properties(CityRealmTests PROPERTIES TIMEOUT 60 LABELS "city;history")
add_executable(CityFabricTests ${CMAKE_CURRENT_LIST_DIR}/CityFabricTests.cpp)
target_link_libraries(CityFabricTests PRIVATE runtime)
target_include_directories(CityFabricTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityFabricTests COMMAND CityFabricTests)
set_tests_properties(CityFabricTests PROPERTIES TIMEOUT 300 LABELS "city;history")
add_executable(CityDefenceTests ${CMAKE_CURRENT_LIST_DIR}/CityDefenceTests.cpp)
target_link_libraries(CityDefenceTests PRIVATE runtime)
target_include_directories(CityDefenceTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityDefenceTests COMMAND CityDefenceTests)
set_tests_properties(CityDefenceTests PROPERTIES TIMEOUT 300 LABELS "city;history")
add_executable(CityChronicleTests ${CMAKE_CURRENT_LIST_DIR}/CityChronicleTests.cpp)
target_link_libraries(CityChronicleTests PRIVATE runtime)
target_include_directories(CityChronicleTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME CityChronicleTests COMMAND CityChronicleTests)
set_tests_properties(CityChronicleTests PROPERTIES TIMEOUT 300 LABELS "city;history")
add_executable(CityScaleTests ${CMAKE_CURRENT_LIST_DIR}/CityScaleTests.cpp)
target_link_libraries(CityScaleTests PRIVATE runtime)
target_include_directories(CityScaleTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME CityScaleTests COMMAND CityScaleTests)
set_tests_properties(CityScaleTests PROPERTIES TIMEOUT 600 LABELS "city;history;scale")
add_executable(CityScaleBenchmark ${CMAKE_SOURCE_DIR}/tools/terrain/CityScaleBenchmark.cpp)
target_link_libraries(CityScaleBenchmark PRIVATE runtime)
target_include_directories(CityScaleBenchmark PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_executable(CityCareerTests ${CMAKE_CURRENT_LIST_DIR}/CityCareerTests.cpp)
target_link_libraries(CityCareerTests PRIVATE runtime)
target_include_directories(CityCareerTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityCareerTests COMMAND CityCareerTests)
set_tests_properties(CityCareerTests PROPERTIES TIMEOUT 300 LABELS "city;history")
add_executable(CityEvolutionPreview ${CMAKE_SOURCE_DIR}/tools/terrain/CityEvolutionPreview.cpp)
target_link_libraries(CityEvolutionPreview PRIVATE Engine)
target_include_directories(CityEvolutionPreview PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_executable(CityHistoricalBriefTests ${CMAKE_CURRENT_LIST_DIR}/CityHistoricalBriefTests.cpp)
target_link_libraries(CityHistoricalBriefTests PRIVATE runtime)
target_include_directories(CityHistoricalBriefTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME CityHistoricalBriefTests COMMAND CityHistoricalBriefTests)
set_tests_properties(CityHistoricalBriefTests PROPERTIES TIMEOUT 120 LABELS "city;history")
# Century-scale preview: optimize the simulation kernels independently of the
# engine's Debug build. The same sources still supply the runtime/test library.
target_sources(CityEvolutionPreview PRIVATE
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityHistory.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityHouseholds.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityEconomy.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityRealm.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityFabric.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityDefence.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityChronicle.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCitySociety.cpp
  ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityScale.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(CityEvolutionPreview PRIVATE -O2)
endif()

add_test(NAME CityEvolutionPreviewSmoke
  COMMAND ${CMAKE_COMMAND}
    -DPREVIEW=$<TARGET_FILE:CityEvolutionPreview>
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/city-evolution-smoke
    -P ${CMAKE_CURRENT_LIST_DIR}/CityEvolutionPreviewSmoke.cmake)
set_tests_properties(CityEvolutionPreviewSmoke PROPERTIES TIMEOUT 120 LABELS "city;history;release")

add_executable(KingdomHistoryTests ${CMAKE_CURRENT_LIST_DIR}/KingdomHistoryTests.cpp)
target_link_libraries(KingdomHistoryTests PRIVATE runtime)
target_include_directories(KingdomHistoryTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME KingdomHistoryTests COMMAND KingdomHistoryTests)
set_tests_properties(KingdomHistoryTests PROPERTIES TIMEOUT 600 LABELS "city;history;kingdom")

add_test(NAME KingdomCenturiesTests COMMAND KingdomHistoryTests --centuries)
set_tests_properties(KingdomCenturiesTests PROPERTIES TIMEOUT 600 LABELS "city;history;kingdom;slow")

add_executable(CityPavingTests ${CMAKE_CURRENT_LIST_DIR}/CityPavingTests.cpp)
target_link_libraries(CityPavingTests PRIVATE runtime)
target_include_directories(CityPavingTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityPavingTests COMMAND CityPavingTests)
