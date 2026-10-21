# cmake/FMinusOptions.cmake: the project's options, the dependency cache and the generator its builds use, ccache,
# Windows' compiler and C runtime, and fminus_target_defaults() (the language level, warnings and sanitizers every
# FMinus target gets; dependencies keep their own flags).

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

# The generator a dependency built at configure time uses (LLVM in cache mode, WinFlexBison): Ninja, this tree's or
# the first on PATH; else Unix Makefiles, or on Windows this tree's own generator (Visual Studio).
function(fminus_dependency_generator out)
  if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_MAKE_PROGRAM)
    set(_gen -G Ninja "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  else()
    find_program(FMINUS_NINJA ninja)
    if(FMINUS_NINJA)
      set(_gen -G Ninja "-DCMAKE_MAKE_PROGRAM=${FMINUS_NINJA}")
    elseif(CMAKE_HOST_WIN32)
      set(_gen -G "${CMAKE_GENERATOR}")
      if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND _gen -A "${CMAKE_GENERATOR_PLATFORM}")
      endif()
      if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND _gen -T "${CMAKE_GENERATOR_TOOLSET}")
      endif()
    else()
      set(_gen -G "Unix Makefiles")
    endif()
  endif()
  set(${out} "${_gen}" PARENT_SCOPE)
endfunction()

# Windows: clang-cl, the MSVC-compatible driver of clang (Visual Studio's "C++ Clang tools for Windows", or LLVM's
# installer); CMakeLists.txt picks it when no compiler was chosen. FMinus needs clang: its tests and examples use
# clang's vector types, which FMinus's float2/float4/float8 match. MSVC's own cl is refused here, where the error is
# clear, instead of in the first test.
set(FMINUS_MSVC_FRONTEND OFF)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(FMINUS_MSVC_FRONTEND ON)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "FMinus: ${CMAKE_CXX_COMPILER} is MSVC's cl; FMinus builds with clang, on Windows clang-cl "
                        "(Visual Studio's C++ Clang tools, or LLVM's installer). Configure a fresh build tree with "
                        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl.")
  endif()
  # The release C runtime (/MD) in every configuration: LLVM's libraries are built against it, and a Debug FMinus
  # cannot link them with the debug one (/MDd). Set before LLVM, Catch2 and CLI11, which follow it.
  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  endif()
  # C++ exceptions (/EHsc, in CMake's default flags): Bison's C++ parser and FMinus's scanner throw. A build tree whose
  # first configure could not find the compiler keeps the empty flags it cached then, which lack them (and /O2).
  if(NOT CMAKE_CXX_FLAGS MATCHES "[/-]EH")
    message(FATAL_ERROR "FMinus: CMAKE_CXX_FLAGS ('${CMAKE_CXX_FLAGS}') has no /EHsc, so C++ exceptions are off, and "
                        "FMinus's parser and scanner throw. A build tree first configured when the compiler could not "
                        "be found keeps the empty flags it cached then: delete ${PROJECT_BINARY_DIR} and configure "
                        "again (the deps cache keeps LLVM).")
  endif()
endif()

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
if(FMINUS_MSVC_FRONTEND)
  # clang-cl reads -Wall as cl's /Wall, which is -Weverything; its /W4 is clang's -Wall -Wextra. The rest are clang's.
  list(REMOVE_ITEM FMINUS_WARNING_FLAGS -Wall -Wextra)
  list(PREPEND FMINUS_WARNING_FLAGS /W4)
endif()

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
if(_fminus_sanitizers AND FMINUS_MSVC_FRONTEND)
  message(FATAL_ERROR "FMinus: FMINUS_SANITIZERS: the sanitizers are not set up for clang-cl (Windows)")
endif()
list(JOIN _fminus_sanitizers "," FMINUS_SANITIZE_ARG)

# The defaults of an FMinus target: C++${FMINUS_CXX_STANDARD} without extensions, the warnings (as errors with
# FMINUS_WERROR), the sanitizers. LLVM, Catch2 and the generated scanners' and parsers' own needs are handled elsewhere.
# The standard is the target's own, never its users': a library says what its headers need itself (fminus_lib's public
# API needs C++17), and an interface library passes the standard on. CXX_STANDARD <n> builds the target in C++<n>
# instead (the public API's test, in C++17).
function(fminus_target_defaults target)
  cmake_parse_arguments(PARSE_ARGV 1 _arg "" "CXX_STANDARD" "")
  if(NOT _arg_CXX_STANDARD)
    set(_arg_CXX_STANDARD ${FMINUS_CXX_STANDARD})
  endif()
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    target_compile_features(${target} INTERFACE cxx_std_${_arg_CXX_STANDARD})
    return()
  endif()
  target_compile_features(${target} PRIVATE cxx_std_${_arg_CXX_STANDARD})
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE ${FMINUS_WARNING_FLAGS} $<$<BOOL:${FMINUS_WERROR}>:-Werror>)
  if(FMINUS_MSVC_FRONTEND)
    # The Windows C runtime's deprecations of standard C (getenv) and POSIX names (fileno): warnings, then errors.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  endif()
  if(FMINUS_SANITIZE_ARG)
    target_compile_options(${target} PRIVATE -fsanitize=${FMINUS_SANITIZE_ARG} -fno-omit-frame-pointer
                                             -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=${FMINUS_SANITIZE_ARG})
  endif()
endfunction()
