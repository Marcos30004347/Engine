cmake_minimum_required(VERSION 3.10)

include(CTest)

include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ThreadLocalStorageTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentLinkedListTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentStackTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentQueueTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentPriorityQueueTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentBoundedDictionary.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentSkipListMapTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentHashMapTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentSortedListTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/datastructures/ConcurrentEpochGarbageCollectorTests.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/async/JobTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/async/AsyncTests.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/rhi/EventLoopTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/rhi/BufferAllocatorTests.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/rendering/RenderGraphTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/rendering/ComputeAddTests.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/rendering/TriangleExample.cmake)