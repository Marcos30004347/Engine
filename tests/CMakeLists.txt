cmake_minimum_required(VERSION 3.10)

include(CTest)

include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ThreadLocalStorageTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentListTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentStackTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentQueueTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentPriorityQueueTests.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/async/ThreadCacheTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/async/JobTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/async/AsyncManagerTests.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/rhi/EventLoopTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/rhi/GPUHeapTests.cmake)