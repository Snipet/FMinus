# FMinus: a JIT compiler for audio DSP. Source text is lexed and parsed by Flex and Bison, lowered to LLVM IR, optimised
# for the CPU it runs on and compiled by ORC's LLJIT, on the user's machine.
#
# Every dependency comes through FetchContent, pinned by hash in cmake/FMinusPins.cmake:
#   LLVM          cmake/FMinusLLVM.cmake         cache (default) | inline | system  -> FMinus::LLVM
#   Bison, Flex   cmake/FMinusParserTools.cmake  fetch (default) | system           -> fminus_add_grammar()
#   CLI11         cmake/FMinusCLI11.cmake        tools only                         -> CLI11::CLI11
#   Catch2, lit   cmake/FMinusTesting.cmake      tests only
# Built dependencies are kept in the deps cache (FMINUS_DEPS_DIR, default .deps/), shared by every build tree.
#
# Layout (README.md): src/ is the fminus library: every .cpp under it is a source, and each Bison grammar there,
# with the Flex scanners beside it, is generated into the library. include/fminus/ is its public API. tools/<name>/
# holds one executable each, examples/<name>/ one example each (a program and its harness). tests/ holds unit/
# (Catch2), smoke/ (the toolchain's own checks) and lit/ (lit and FileCheck). Adding a file edits no CMake.
#
# Use the presets: `cmake --workflow --preset dev` configures, builds and tests (CMakePresets.json lists the others).

cmake_minimum_required(VERSION 3.30)    # CMP0168: FetchContent populates directly, without a sub-build

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")

# Windows: clang-cl unless a compiler was chosen (CC, CXX, -DCMAKE_<LANG>_COMPILER). FMinus needs clang there too
# (cmake/FMinusOptions.cmake). A Visual Studio generator picks it with -T ClangCL instead.
if(CMAKE_HOST_WIN32 AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang-cl)
  endif()
  if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang-cl)
  endif()
endif()

project(FMinus
  VERSION 0.1.0
  DESCRIPTION "A JIT compiler for audio DSP on LLVM"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(FMinusPins)
include(FMinusOptions)          # options, the deps cache, ccache, fminus_target_defaults()
include(FMinusParserTools)      # Bison, Flex, fminus_add_grammar() (first: a minute, where LLVM can take half an hour)
include(FMinusLLVM)             # FMinus::LLVM

# Our own outputs only (set after LLVM, which places its own).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

if(FMINUS_LLVM_MODE STREQUAL "system")
  set(_fminus_asserts ${LLVM_ENABLE_ASSERTIONS})
else()
  set(_fminus_asserts ${FMINUS_LLVM_ASSERTIONS})
endif()
if(_fminus_asserts)
  set(FMINUS_LLVM_ASSERTIONS_01 1)
else()
  set(FMINUS_LLVM_ASSERTIONS_01 0)
endif()
configure_file(cmake/templates/Config.h.in "${PROJECT_BINARY_DIR}/include/fminus/Config.h" @ONLY)

if(FMINUS_BUILD_TESTS)
  enable_testing()              # before the examples, which add their --check tests
endif()
add_subdirectory(src)
if(FMINUS_BUILD_TOOLS OR FMINUS_BUILD_EXAMPLES)
  include(FMinusCLI11)          # CLI11::CLI11, for the tools and the examples
endif()
set(FMINUS_TOOL_TARGETS "")
if(FMINUS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
set(FMINUS_EXAMPLE_TARGETS "")
if(FMINUS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(FMINUS_BUILD_TESTS)
  include(FMinusTesting)
  add_subdirectory(tests)
endif()

message(STATUS "FMinus ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE}, C++${FMINUS_CXX_STANDARD}, "
               "sanitizers '${FMINUS_SANITIZERS}', deps cache ${FMINUS_DEPS_DIR}")
