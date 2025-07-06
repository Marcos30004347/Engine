cmake_minimum_required(VERSION 3.10)

include(CTest)

include(${CMAKE_CURRENT_SOURCE_DIR}/lib/ThreadLocalStorageTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lib/ConcurrentListTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lib/ConcurrentStackTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lib/ConcurrentQueueTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lib/ConcurrentPriorityQueueTests.cmake)
#include(${CMAKE_CURRENT_SOURCE_DIR}/lib/CPQ.cmake)
