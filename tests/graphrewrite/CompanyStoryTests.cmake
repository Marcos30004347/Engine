add_test(NAME CompanyStoryTests COMMAND CompanyStoryTests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME CompanyStoryCLI
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/company_cli_test.py $<TARGET_FILE:company-story>)
endif()
