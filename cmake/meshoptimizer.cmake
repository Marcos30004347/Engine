include(FetchContent)

if(NOT TARGET meshoptimizer)
    message(STATUS "Fetching meshoptimizer...")
    
    FetchContent_Declare(
        meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG        v1.0.1
    )
    
    FetchContent_MakeAvailable(meshoptimizer)
    
    target_include_directories(meshoptimizer INTERFACE
        $<BUILD_INTERFACE:${meshoptimizer_SOURCE_DIR}/src>
    )
    
    message(STATUS "meshoptimizer fetched successfully")
endif()