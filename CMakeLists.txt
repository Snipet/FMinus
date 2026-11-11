# FMinus: a JIT compiler for audio DSP. Source text is lexed and parsed by Flex and Bison, lowered to LLVM IR, optimised
# for the CPU it runs on and compiled by ORC's LLJIT, on the user's machine.
#
# Every dependency comes through FetchContent, pinned by hash in cmake/FMinusPins.cmake:
#   LLVM          cmake/FMinusLLVM.cmake         cache (default) | inline | system  -> FMinus::LLVM
#   Bison, Flex   cmake/FMinusParserTools.cmake  fetch (default) | system           -> fminus_add_grammar()
#   PFFFT         cmake/FMinusFft.cmake          two C files, into the runtime     -> fminus_pffft
#   fast_float    cmake/FMinusFastFloat.cmake    headers, for the runtime's parsers -> fminus_fast_float
#                                                (FMINUS_JSON or FMINUS_XML)
#   yyjson        cmake/FMinusJson.cmake         one C file, into the runtime       -> fminus_yyjson
#                                                (FMINUS_JSON)
#   pugixml       cmake/FMinusXml.cmake          one C++ file, into the runtime     -> fminus_pugixml
#                                                (FMINUS_XML)
#   libcurl,      cmake/FMinusWeb.cmake          built at configure time into the  -> fminus_web_curl,
#   Mbed TLS                                     deps cache (FMINUS_WEB)              fminus_web_mbedtls
#   CLI11         cmake/FMinusCLI11.cmake        tools only                         -> CLI11::CLI11
#   Catch2, lit   cmake/FMinusTesting.cmake      tests only
#   pybind11      cmake/FMinusPython.cmake       the Python package only (FMINUS_BUILD_PYTHON)
# Built dependencies are kept in the deps cache (FMINUS_DEPS_DIR, default .deps/), shared by every build tree.
#
# Layout (README.md): src/ is the fminus library: every .cpp under it is a source, and each Bison grammar there,
# with the Flex scanners beside it, is generated into the library. include/fminus/ is its public API. tools/<name>/
# holds one executable each, examples/<name>/ one example each (a program and its harness). tests/ holds unit/
# (Catch2), smoke/ (the toolchain's own checks) and lit/ (lit and FileCheck). Adding a file edits no CMake.
# python/ is the Python package, `import fminus` (docs/python.md), built when FMINUS_BUILD_PYTHON is ON.
#
# Use the presets: `cmake --workflow --preset dev` configures, builds and tests (CMakePresets.json lists the others).

cmake_minimum_required(VERSION 3.30)    # CMP0168: FetchContent populates directly, without a sub-build

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")

# Windows: clang-cl, from a developer prompt, unless a compiler was chosen (CC, CXX, -DCMAKE_<LANG>_COMPILER). A Visual
# Studio generator picks it with -T ClangCL instead.
if(CMAKE_HOST_WIN32 AND NOT CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT DEFINED CMAKE_C_COMPILER
   AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CC} AND NOT DEFINED ENV{CXX})
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/FMinusClangCl.cmake")
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
include(FMinusFft)              # PFFFT, compiled into the runtime library
include(FMinusFastFloat)        # fast_float, the parsers' numbers, when FMINUS_JSON or FMINUS_XML is on
include(FMinusJson)             # yyjson, the JSON parser, compiled into the runtime library when FMINUS_JSON is on
include(FMinusXml)              # pugixml, the XML parser, compiled into the runtime library when FMINUS_XML is on
include(FMinusWeb)              # libcurl and Mbed TLS, for the web library fminus_web, when FMINUS_WEB is on

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
foreach(_feature JSON XML WEB)
  if(FMINUS_${_feature})
    set(FMINUS_HAS_${_feature} 1)
  else()
    set(FMINUS_HAS_${_feature} 0)
  endif()
endforeach()
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

if(FMINUS_BUILD_PYTHON)
  include(FMinusPython)         # the Python it is built for, and pybind11
  add_subdirectory(python)      # after the tests: its own run under ctest too
endif()

message(STATUS "FMinus ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE}, C++${FMINUS_CXX_STANDARD}, "
               "sanitizers '${FMINUS_SANITIZERS}', deps cache ${FMINUS_DEPS_DIR}")
