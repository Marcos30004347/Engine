project(graphrewritec LANGUAGES CXX)

add_executable(graphrewritec main.cpp)
target_link_libraries(graphrewritec PRIVATE runtime)

add_executable(graphrewrite-story story.cpp)
target_link_libraries(graphrewrite-story PRIVATE Engine)

# --gpu needs the graph-rewrite SPIR-V next to the executable: the shaders are
# built into the tests' asset tree, and readRelativeFile resolves against the
# executable's directory, so link that tree in here.
add_dependencies(graphrewrite-story graphrewrite_rewrite_cs graphrewrite_match_cs graphrewrite_publish_cs)
add_custom_command(TARGET graphrewrite-story POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:graphrewrite-story>/assets/shaders
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv $<TARGET_FILE_DIR:graphrewrite-story>/assets/shaders/spirv
    COMMENT "Linking graph-rewrite shaders beside graphrewrite-story")

# The terminal uses the same compiled graph-rewrite runtime as the story generator.
add_library(company-simulation STATIC company/Company.cpp company/Architecture.cpp company/Planning.cpp company/Rules.cpp company/GraphRenderer.cpp company/SystemDesign.cpp)
target_link_libraries(company-simulation PUBLIC runtime)
target_compile_definitions(company-simulation PRIVATE COMPANY_RULES_PATH="${CMAKE_SOURCE_DIR}/assets/graphrewrite/company.graphrewrite")
target_compile_features(company-simulation PUBLIC cxx_std_17)
target_include_directories(company-simulation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/company)
add_library(company-terminal STATIC company/CompanyCli.cpp)
target_link_libraries(company-terminal PUBLIC company-simulation)
add_executable(company-story company/main.cpp)
target_link_libraries(company-story PRIVATE company-terminal)
target_link_libraries(graphrewrite-story PRIVATE company-terminal)
add_executable(CompanyStoryTests ${CMAKE_SOURCE_DIR}/tests/graphrewrite/CompanyStoryTests.cpp)
target_link_libraries(CompanyStoryTests PRIVATE company-simulation)
