cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED ENGINE_SOURCE_DIR)
  message(FATAL_ERROR "ENGINE_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE REPOSITORY_FILES LIST_DIRECTORIES false "${ENGINE_SOURCE_DIR}/*")
set(LEGACY_EXTENSION_LOWER "ev")
string(APPEND LEGACY_EXTENSION_LOWER "xl")
foreach(REPOSITORY_FILE IN LISTS REPOSITORY_FILES)
  file(RELATIVE_PATH RELATIVE_FILE "${ENGINE_SOURCE_DIR}" "${REPOSITORY_FILE}")
  string(TOLOWER "${RELATIVE_FILE}" LOWER_FILE)
  if(LOWER_FILE MATCHES "^(build|thirdparty|\\.git)/" OR
     LOWER_FILE STREQUAL "docs/sparse-voxel-rasterization-migration-plan.md")
    continue()
  endif()
  if(LOWER_FILE MATCHES "${LEGACY_EXTENSION_LOWER}")
    message(FATAL_ERROR "G13 legacy asset/name survived: ${RELATIVE_FILE}")
  endif()
endforeach()

string(TOUPPER "${LEGACY_EXTENSION_LOWER}" LEGACY_EXTENSION)
set(LEGACY_CONTENT_PATTERN "${LEGACY_EXTENSION}|\\.${LEGACY_EXTENSION_LOWER}|readVoxel")
string(APPEND LEGACY_CONTENT_PATTERN "SceneFile|writeVoxel")
string(APPEND LEGACY_CONTENT_PATTERN "SceneFile|VoxelSceneFileComp")
string(APPEND LEGACY_CONTENT_PATTERN "ression|VoxelVirtualScene")
string(APPEND LEGACY_CONTENT_PATTERN "File|VoxelCulling")
string(APPEND LEGACY_CONTENT_PATTERN "Scene|VoxelCullingInst")
string(APPEND LEGACY_CONTENT_PATTERN "anceGPU|VoxelCullingBrick")
string(APPEND LEGACY_CONTENT_PATTERN "GPU|VoxelBVH8Node")
string(APPEND LEGACY_CONTENT_PATTERN "GPU|GPUSparseVox")
string(APPEND LEGACY_CONTENT_PATTERN "el")
execute_process(
  COMMAND git grep -n -I -i -E "${LEGACY_CONTENT_PATTERN}" -- .
          ":(exclude)docs/sparse-voxel-rasterization-migration-plan.md"
          ":(exclude)thirdparty/**"
  WORKING_DIRECTORY "${ENGINE_SOURCE_DIR}"
  RESULT_VARIABLE GREP_RESULT
  OUTPUT_VARIABLE GREP_OUTPUT
  ERROR_VARIABLE GREP_ERROR
)
if(GREP_RESULT EQUAL 0)
  message(FATAL_ERROR "G13 legacy source/API references survived:\n${GREP_OUTPUT}")
elseif(NOT GREP_RESULT EQUAL 1)
  message(FATAL_ERROR "G13 repository audit failed to run: ${GREP_ERROR}")
endif()

message(STATUS "G13 legacy voxel removal audit passed")
