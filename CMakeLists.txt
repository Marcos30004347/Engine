cmake_minimum_required(VERSION 3.10)

include(cmake/WGSL2SpirV.cmake)

project(Engine)


# option(ENABLE_PROFILING "Enable CPU profiling with gperftools" ON)

#if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Set default build type to Debug" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer -g -O0")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer -g -O0")
#endif()

    # set(CMAKE_BUILD_TYPE Debug CACHE STRING "Set default build type to Debug" FORCE)
    # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
    # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")

# set(CMAKE_BUILD_TYPE Debug CACHE STRING "Set default build type to Debug" FORCE)
# set(CMAKE_CXX_FLAGS "-g -O0 -fsanitize=thread -fsanitize=undefined -fno-omit-frame-pointer")
# set(CMAKE_C_FLAGS "-g -O0 -fsanitize=thread -fsanitize=undefined -fno-omit-frame-pointer")

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -fno-omit-frame-pointer")
# set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -O2 -g -fno-omit-frame-pointer")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -g -O0")
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -g -O0")

#Comment this for debug builds
# if(NOT CMAKE_BUILD_TYPE)
#   set(CMAKE_BUILD_TYPE Release CACHE STRING "Set default build type to Release" FORCE)
# endif()
# if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#     set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -flto -fvisibility=hidden")
#     set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG -flto -fvisibility=hidden")
# elseif(MSVC)
#     set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Ox /DNDEBUG /GL")
#     set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Ox /DNDEBUG /GL")
# endif()


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#fcontext
add_subdirectory(thirdparty/fcontext)
add_subdirectory(thirdparty/SDL)

add_library(simdpp INTERFACE)
target_include_directories(simdpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libsimdpp)

find_package(Vulkan QUIET)

if (Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
    set(HAS_VULKAN TRUE)
else()
    message(WARNING "Vulkan SDK not found. Vulkan backend will not be built.")
    set(HAS_VULKAN FALSE)
endif()


file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rpmalloc/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap/o1heap/*.c"
)

message(${SOURCES})

add_library(Engine ${SOURCES})

if (HAS_VULKAN)
    target_include_directories(Engine PRIVATE ${Vulkan_INCLUDE_DIR})
    target_link_libraries(Engine PRIVATE Vulkan::Vulkan)
endif()

if(APPLE)
    set(VULKAN_SDK_PATH $ENV{VULKAN_SDK})

    if (VULKAN_SDK_PATH)
        message(STATUS "Vulkan SDK found at: ${VULKAN_SDK_PATH}")
    else()
        message(WARNING "VULKAN_SDK environment variable is not set!")
    endif()

    set(MOLTENVK_LIB "${VULKAN_SDK_PATH}/lib/libMoltenVK.dylib")
    target_link_libraries(Engine PRIVATE ${MOLTENVK_LIB})
endif()

if(ENABLE_PROFILING)
    find_library(PROFILER_LIB profiler)
    if(PROFILER_LIB)
        message(STATUS "gperftools profiler found: ${PROFILER_LIB}")
        target_link_libraries(Engine PRIVATE ${PROFILER_LIB})
        target_compile_definitions(Engine PRIVATE ENABLE_PROFILING)
    else()
        message(WARNING "gperftools profiler not found, disabling profiling support")
    endif()
endif()

# if(CMAKE_BUILD_TYPE STREQUAL "Debug")
#   target_compile_definitions(Engine PRIVATE DEBUG)
#   target_compile_definitions(Engine PRIVATE NDEBUG)
# endif()

target_compile_definitions(Engine PRIVATE ENABLE_OVERRIDE=0)
target_compile_definitions(Engine PRIVATE SDL3_AVAILABLE)
target_include_directories(Engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(Engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/fcontext/include)
target_include_directories(Engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
target_include_directories(Engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap)

target_link_libraries(Engine 
    PUBLIC fcontext
    PUBLIC SDL3::SDL3
    PUBLIC simdpp
)

target_include_directories(Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/)

if(WIN32)    
    set(DLLS
        "${CMAKE_BINARY_DIR}/thirdparty/SDL/SDL3.dll"
    )

    set_target_properties(Engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    foreach(DLL ${DLLS})
        add_custom_command(TARGET Engine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL}"
                "$<TARGET_FILE_DIR:Engine>"
        )
    endforeach()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(Engine PRIVATE VULKAN_DEVICE_LOG)
endif()

enable_testing()
add_subdirectory(tests)

