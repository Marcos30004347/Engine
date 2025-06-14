# PROJECT: fcontext
cmake_minimum_required(VERSION 3.5...3.16)
project(fcontext C)

option(BUILD_TEST "build test app" 0)

if (NOT CMAKE_MODULE_PATH)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
endif()

if(MSVC)
  add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
  add_definitions(-D_HAS_EXCEPTIONS=0)
endif()

set(HEADER "include/fcontext/fcontext.h")
set(SOURCES "source/stack.c")

if(WIN32)
  set(_default_binfmt pe)
elseif(APPLE)
  set(_default_binfmt mach-o)
else()
  set(_default_binfmt elf)
endif()

set(BOOST_CONTEXT_BINARY_FORMAT "${_default_binfmt}" CACHE STRING "Boost.Context binary format (elf, mach-o, pe, xcoff)")
set_property(CACHE BOOST_CONTEXT_BINARY_FORMAT PROPERTY STRINGS elf mach-o pe xcoff)

unset(_default_binfmt)

math(EXPR _bits "${CMAKE_SIZEOF_VOID_P}*8")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  set(_default_abi aapcs)
elseif(WIN32)
  set(_default_abi ms)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^mips")
  if(_bits EQUAL 32)
    set(_default_abi o32)
  else()
    set(_default_abi n64)
  endif()
else()
  set(_default_abi sysv)
endif()

set(BOOST_CONTEXT_ABI "${_default_abi}" CACHE STRING "Boost.Context ABI (aapcs, eabi, ms, n32, n64, o32, o64, sysv, x32)")
set_property(CACHE BOOST_CONTEXT_ABI PROPERTY STRINGS aapcs eabi ms n32 n64 o32 o64 sysv x32)

unset(_default_abi)

## Arch-and-model

set(_all_archs arm arm64 loongarch64 mips32 mips64 power riscv64 s390x i386 x86_64 combined)

# Try at start to auto determine arch from CMake.
if(CMAKE_SYSTEM_PROCESSOR IN_LIST _all_archs)
  set(_default_arch ${CMAKE_SYSTEM_PROCESSOR})
elseif(_bits EQUAL 32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]")
    set(_default_arch arm)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^mips")
    set(_default_arch mips32)
  else()
    set(_default_arch i386)
  endif()
else()
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]") # armv8
    set(_default_arch arm64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^mips")
    set(_default_arch mips64)
  else()
    set(_default_arch x86_64)
  endif()
endif()

set(BOOST_CONTEXT_ARCHITECTURE "${_default_arch}" CACHE STRING "Boost.Context architecture (arm, arm64, loongarch64, mips32, mips64, power, riscv64, s390x, i386, x86_64, combined)")
set_property(CACHE BOOST_CONTEXT_ARCHITECTURE PROPERTY STRINGS ${_all_archs})

unset(_all_archs)
unset(_bits)
unset(_default_arch)

if(MSVC)
  if(BOOST_CONTEXT_ARCHITECTURE STREQUAL arm64 OR BOOST_CONTEXT_ARCHITECTURE STREQUAL arm)
    set(_default_asm armasm)
  else()
    set(_default_asm masm)
  endif()
elseif(BOOST_CONTEXT_ARCHITECTURE STREQUAL arm64 AND MINGW)
  set(_default_asm armclang)
else()
  set(_default_asm gas)
endif()

set(BOOST_CONTEXT_ASSEMBLER "${_default_asm}" CACHE STRING "Boost.Context assembler (masm, gas, armasm, armclang)")
set_property(CACHE BOOST_CONTEXT_ASSEMBLER PROPERTY STRINGS masm gas armasm armclang)

unset(_default_asm)

## Assembler source suffix
message("BOOST_CONTEXT_ASSEMBLER = ${BOOST_CONTEXT_ASSEMBLER}")

if(BOOST_CONTEXT_ASSEMBLER STREQUAL armclang)
  set(_default_ext .S)
elseif(BOOST_CONTEXT_BINARY_FORMAT STREQUAL pe)
  set(_default_ext .asm)
elseif(BOOST_CONTEXT_ASSEMBLER STREQUAL gas)
  set(_default_ext .S)
else()
  set(_default_ext .asm)
endif()
message("_default_ext = ${_default_ext}")

set(BOOST_CONTEXT_ASM_SUFFIX "${_default_ext}" CACHE STRING "Boost.Context assembler source suffix (.asm, .S)")
set_property(CACHE BOOST_CONTEXT_ASM_SUFFIX PROPERTY STRINGS .asm .S)

unset(_default_ext)

## Implementation

set(_default_impl fcontext)

set(BOOST_CONTEXT_IMPLEMENTATION "${_default_impl}" CACHE STRING "Boost.Context implementation (fcontext, ucontext, winfib)")
set_property(CACHE BOOST_CONTEXT_IMPLEMENTATION PROPERTY STRINGS fcontext ucontext winfib)

unset(_default_impl)

#

message(STATUS "fcontext: "
  "architecture ${BOOST_CONTEXT_ARCHITECTURE}, "
  "binary format ${BOOST_CONTEXT_BINARY_FORMAT}, "
  "ABI ${BOOST_CONTEXT_ABI}, "
  "assembler ${BOOST_CONTEXT_ASSEMBLER}, "
  "suffix ${BOOST_CONTEXT_ASM_SUFFIX}, "
  "implementation ${BOOST_CONTEXT_IMPLEMENTATION}")



# Enable the right assembler
set(ASM_LANGUAGE)
if(BOOST_CONTEXT_IMPLEMENTATION STREQUAL "fcontext")
  if(BOOST_CONTEXT_ASSEMBLER STREQUAL gas OR BOOST_CONTEXT_ASSEMBLER STREQUAL armclang)
    if(CMAKE_CXX_PLATFORM_ID MATCHES "Cygwin")
      set(ASM_LANGUAGE ASM-ATT)
    else()
      set(ASM_LANGUAGE ASM)
    endif()
  elseif(BOOST_CONTEXT_ASSEMBLER STREQUAL armasm)
    set(ASM_LANGUAGE ASM_ARMASM)
  else()
    set(ASM_LANGUAGE ASM_MASM)
  endif()

  enable_language(${ASM_LANGUAGE})
endif()

# Choose .asm sources
if(BOOST_CONTEXT_BINARY_FORMAT STREQUAL mach-o)
  set(BOOST_CONTEXT_BINARY_FORMAT macho)
endif()

set(_asm_suffix ${BOOST_CONTEXT_ARCHITECTURE}_${BOOST_CONTEXT_ABI}_${BOOST_CONTEXT_BINARY_FORMAT}_${BOOST_CONTEXT_ASSEMBLER}${BOOST_CONTEXT_ASM_SUFFIX})

set(ASM_SOURCES
  asm/make_${_asm_suffix}
  asm/jump_${_asm_suffix}
  asm/ontop_${_asm_suffix}
)
message("ASM SOURCES = ${ASM_SOURCES}" )

unset(_asm_suffix)

if(BOOST_CONTEXT_ASSEMBLER STREQUAL masm)
  set_property(SOURCE ${ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/nologo")
  if(MSVC AND NOT(MSVC_VERSION LESS 1936) AND NOT(CMAKE_CXX_SIMULATE_VERSION))
    set_property(SOURCE ${ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/quiet")
  endif()
  if(BOOST_CONTEXT_ARCHITECTURE STREQUAL i386)
    set_property(SOURCE ${ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/safeseh")
  endif()
else() 
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE ${ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-x" "assembler-with-cpp")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_property(SOURCE ${ASM_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-Wno-unused-command-line-argument")
  endif()
endif() 

enable_language(${ASM_LANGUAGE})

set_source_files_properties(${ASM_SOURCES} PROPERTIES LANGUAGE ${ASM_LANGUAGE})

add_library(fcontext STATIC ${SOURCES} ${ASM_SOURCES})

target_include_directories(fcontext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(fcontext PROPERTIES FOLDER Deps ${IOS_GENERAL_PROPERTIES})

if (BUILD_TEST)
    add_executable(test_fcontext test/test_fcontext.c)
    target_link_libraries(test_fcontext fcontext)
endif()

