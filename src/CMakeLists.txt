# src/CMakeLists.txt: the fminus library (target fminus_lib, libfminus.a, FMinus::FMinus), the FMinus runtime
# (FMinus::Runtime), and FMinus::Internal for this tree's tools and tests. (The library's target is fminus_lib so the
# driver, tools/fminus/, can be the target fminus.)
#
# fminus_runtime: every .cpp under src/runtime/, and PFFFT's objects (the FFT). It holds the C functions generated code
# calls, with no LLVM in it. A process that JITs links it (through fminus), and executables from support/Aot.h link
# libfminus_runtime.a itself.
#
# fminus_lib: every other .cpp under src/ (globbed: adding a file edits no CMake). Every directory holding a Bison
# grammar (*.y) is a grammar: that .y, with every Flex scanner (*.l) beside it, is generated into the library through
# fminus_add_grammar. A scanner in a directory without a grammar is generated alone. Generated files land under
# ${CMAKE_CURRENT_BINARY_DIR}/generated/<dir>/; include them as "<dir>/<stem>.tab.hpp" and "<dir>/<stem>.yy.hpp".
#
# FMinus::FMinus is the library as a host uses it: its public API, include/fminus/FMinus.h, which needs C++17 and has
# no LLVM in it. A host gets the include roots include/ ("fminus/FMinus.h") and the generated include/fminus/Config.h,
# and links LLVM, but compiles with neither LLVM's include paths and definitions nor FMinus's C++23. FMinus::Internal,
# for this tree's tools, examples and tests, adds both, src/ ("support/LLVMInit.h", "runtime/Runtime.h",
# "frontend/...") and the generated grammar files.

file(GLOB_RECURSE _fminus_all_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.def")
set(_fminus_runtime_sources ${_fminus_all_sources})
list(FILTER _fminus_runtime_sources INCLUDE REGEX "^runtime/")
set(_fminus_sources ${_fminus_all_sources})
list(FILTER _fminus_sources EXCLUDE REGEX "^runtime/")
list(TRANSFORM _fminus_runtime_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM _fminus_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
file(GLOB_RECURSE _fminus_public_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/*.h")

# ---- the runtime ----------------------------------------------------------------------------------------------------
add_library(fminus_runtime STATIC ${_fminus_runtime_sources})
add_library(FMinus::Runtime ALIAS fminus_runtime)
fminus_target_defaults(fminus_runtime)
target_include_directories(fminus_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# The FFT, PFFFT (cmake/FMinusFft.cmake): its objects go into this library, so it holds the FFT itself.
target_sources(fminus_runtime PRIVATE $<TARGET_OBJECTS:fminus_pffft>)
target_include_directories(fminus_runtime SYSTEM PRIVATE "${FMINUS_PFFFT_INCLUDE_DIR}")
target_compile_definitions(fminus_runtime PRIVATE PFFFT_STATIC_DEFINE)
# The pool behind parallel for and parallel { } (runtime/Parallel.cpp) makes threads: the system's thread library, on a
# system where it is one of its own.
find_package(Threads REQUIRED)
target_link_libraries(fminus_runtime PUBLIC Threads::Threads)

# ---- the library ----------------------------------------------------------------------------------------------------
add_library(fminus_lib STATIC ${_fminus_sources} ${_fminus_public_headers})
add_library(FMinus::FMinus ALIAS fminus_lib)
# libfminus.a, and on Windows libfminus.lib: fminus.lib there is the driver's (tools/fminus) import library, which the
# linker writes into lib/ too, since LLVM's JIT exports two symbols for debuggers (__jit_debug_register_code).
if(MSVC)
  set_target_properties(fminus_lib PROPERTIES OUTPUT_NAME libfminus)
else()
  set_target_properties(fminus_lib PROPERTIES OUTPUT_NAME fminus)
endif()
fminus_target_defaults(fminus_lib)
target_include_directories(fminus_lib
  PUBLIC "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fminus_lib INTERFACE cxx_std_17)   # what include/fminus/FMinus.h needs
target_link_libraries(fminus_lib PUBLIC fminus_runtime PRIVATE FMinus::LLVM)   # linked by its users, not included
# What support/Aot.cpp links executables with: this tree's C++ compiler driver, runtime library and sanitizers.
target_compile_definitions(fminus_lib PRIVATE
  "FMINUS_AOT_DRIVER=\"${CMAKE_CXX_COMPILER}\""
  "FMINUS_RUNTIME_LIBRARY=\"$<TARGET_FILE:fminus_runtime>\""
  "FMINUS_AOT_SANITIZE=\"${FMINUS_SANITIZE_ARG}\"")

file(GLOB_RECURSE _fminus_grammars CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.y")
file(GLOB_RECURSE _fminus_scanners CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.l")
set(_fminus_grammar_dirs "")
foreach(_y IN LISTS _fminus_grammars)
  cmake_path(GET _y PARENT_PATH _dir)
  if(_dir IN_LIST _fminus_grammar_dirs)
    message(FATAL_ERROR "FMinus: ${_dir} holds more than one grammar (*.y); give each its own directory")
  endif()
  list(APPEND _fminus_grammar_dirs "${_dir}")
  set(_lexers "")
  foreach(_l IN LISTS _fminus_scanners)
    cmake_path(GET _l PARENT_PATH _ldir)
    if(_ldir STREQUAL _dir)
      list(APPEND _lexers "${_l}")
    endif()
  endforeach()
  fminus_add_grammar(fminus_lib PARSER "${_y}" LEXERS ${_lexers} SCOPE PRIVATE)
endforeach()
foreach(_l IN LISTS _fminus_scanners)
  cmake_path(GET _l PARENT_PATH _ldir)
  if(NOT _ldir IN_LIST _fminus_grammar_dirs)
    fminus_add_grammar(fminus_lib LEXERS "${_l}" SCOPE PRIVATE)
  endif()
endforeach()

# This tree's tools and tests see the library's internals too.
add_library(fminus_internal INTERFACE)
add_library(FMinus::Internal ALIAS fminus_internal)
fminus_target_defaults(fminus_internal)   # C++${FMINUS_CXX_STANDARD}, which the internal headers need
target_link_libraries(fminus_internal INTERFACE fminus_lib FMinus::LLVM)
target_include_directories(fminus_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                     "${CMAKE_CURRENT_BINARY_DIR}/generated")
