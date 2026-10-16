# cmake/FMinusOptions.cmake: the project's options, the dependency cache, ccache, and fminus_target_defaults() (the
# language level, warnings and sanitizers every FMinus target gets; dependencies keep their own flags).

option(FMINUS_BUILD_TESTS "Build the Catch2 and lit tests" ${PROJECT_IS_TOP_LEVEL})
option(FMINUS_BUILD_TOOLS "Build the executables under tools/" ${PROJECT_IS_TOP_LEVEL})
option(FMINUS_BUILD_EXAMPLES "Build the examples under examples/" ${PROJECT_IS_TOP_LEVEL})
option(FMINUS_WERROR "Treat warnings in FMinus's own sources as errors" ON)
set(FMINUS_CXX_STANDARD 23 CACHE STRING "C++ standard of FMinus's own targets")
set(FMINUS_SANITIZERS "" CACHE STRING "Sanitizers for FMinus's own targets: a list of address, undefined, thread")

# The dependency cache: extracted sources, build trees and installs, shared by every build tree of this checkout (and
# of others, when FMINUS_DEPS_DIR points them at one place). Safe to delete; the next configure rebuilds what it needs.
# ${FMINUS_DEPS_DIR}/build only speeds up rebuilding a flavor and can go once everything is installed. (FetchContent
# keeps each tarball in the build tree that downloaded it, under _deps/<name>-tmp/: it has no shared download
# directory.)
if(DEFINED ENV{FMINUS_DEPS_DIR})
  set(_fminus_deps_default "$ENV{FMINUS_DEPS_DIR}")
else()
  set(_fminus_deps_default "${PROJECT_SOURCE_DIR}/.deps")
endif()
set(FMINUS_DEPS_DIR "${_fminus_deps_default}" CACHE PATH "Dependency cache (sources, builds, installs)")
cmake_host_system_information(RESULT _fminus_cores QUERY NUMBER_OF_LOGICAL_CORES)
set(FMINUS_DEPS_JOBS "${_fminus_cores}" CACHE STRING "Parallel jobs when building a dependency at configure time")
file(MAKE_DIRECTORY "${FMINUS_DEPS_DIR}/src" "${FMINUS_DEPS_DIR}/build" "${FMINUS_DEPS_DIR}/install")

# ccache or sccache, when installed and no launcher was chosen. Set before LLVM so an inline LLVM uses it too.
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(FMINUS_CCACHE NAMES ccache sccache)
  if(FMINUS_CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${FMINUS_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${FMINUS_CCACHE}")
  endif()
endif()

set(FMINUS_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wnull-dereference
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)

set(_fminus_sanitizers "")
foreach(_s IN LISTS FMINUS_SANITIZERS)
  if(NOT _s MATCHES "^(address|undefined|thread)$")
    message(FATAL_ERROR "FMinus: FMINUS_SANITIZERS: unknown sanitizer '${_s}' (address, undefined, thread)")
  endif()
  list(APPEND _fminus_sanitizers "${_s}")
endforeach()
if("thread" IN_LIST _fminus_sanitizers AND "address" IN_LIST _fminus_sanitizers)
  message(FATAL_ERROR "FMinus: FMINUS_SANITIZERS: address and thread cannot be combined")
endif()
list(JOIN _fminus_sanitizers "," FMINUS_SANITIZE_ARG)

# The defaults of an FMinus target: C++${FMINUS_CXX_STANDARD} without extensions, the warnings (as errors with
# FMINUS_WERROR), the sanitizers. LLVM, Catch2 and the generated scanners' and parsers' own needs are handled elsewhere.
function(fminus_target_defaults target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    target_compile_features(${target} INTERFACE cxx_std_${FMINUS_CXX_STANDARD})
    return()
  endif()
  target_compile_features(${target} PUBLIC cxx_std_${FMINUS_CXX_STANDARD})
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE ${FMINUS_WARNING_FLAGS} $<$<BOOL:${FMINUS_WERROR}>:-Werror>)
  if(FMINUS_SANITIZE_ARG)
    target_compile_options(${target} PRIVATE -fsanitize=${FMINUS_SANITIZE_ARG} -fno-omit-frame-pointer
                                             -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=${FMINUS_SANITIZE_ARG})
  endif()
endfunction()
