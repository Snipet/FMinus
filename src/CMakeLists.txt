# src/CMakeLists.txt: the fminus library (FMinus::FMinus), the FMinus runtime (FMinus::Runtime), and FMinus::Internal
# for this tree's tools and tests.
#
# fminus_runtime: every .cpp under src/runtime/. It holds the C functions generated code calls, with no LLVM in it. A
# process that JITs links it (through fminus), and executables from support/Aot.h link libfminus_runtime.a itself.
#
# fminus: every other .cpp under src/ (globbed: adding a file edits no CMake). Every directory holding a Bison grammar
# (*.y) is a grammar: that .y, with every Flex scanner (*.l) beside it, is generated into the library through
# fminus_add_grammar. A scanner in a directory without a grammar is generated alone. Generated files land under
# ${CMAKE_CURRENT_BINARY_DIR}/generated/<dir>/; include them as "<dir>/<stem>.tab.hpp" and "<dir>/<stem>.yy.hpp".
#
# Include roots: include/ (public: "fminus/..."), the generated include/fminus/Config.h, and for FMinus::Internal also
# src/ ("support/LLVMInit.h", "runtime/Runtime.h", "frontend/...") and the generated grammar files.

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

# ---- the library ----------------------------------------------------------------------------------------------------
add_library(fminus STATIC ${_fminus_sources} ${_fminus_public_headers})
add_library(FMinus::FMinus ALIAS fminus)
fminus_target_defaults(fminus)
target_include_directories(fminus
  PUBLIC "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fminus PUBLIC FMinus::LLVM fminus_runtime)
# What support/Aot.cpp links executables with: this tree's C++ compiler driver, runtime library and sanitizers.
target_compile_definitions(fminus PRIVATE
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
  fminus_add_grammar(fminus PARSER "${_y}" LEXERS ${_lexers} SCOPE PRIVATE)
endforeach()
foreach(_l IN LISTS _fminus_scanners)
  cmake_path(GET _l PARENT_PATH _ldir)
  if(NOT _ldir IN_LIST _fminus_grammar_dirs)
    fminus_add_grammar(fminus LEXERS "${_l}" SCOPE PRIVATE)
  endif()
endforeach()

# This tree's tools and tests see the library's internals too.
add_library(fminus_internal INTERFACE)
add_library(FMinus::Internal ALIAS fminus_internal)
target_link_libraries(fminus_internal INTERFACE fminus)
target_include_directories(fminus_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                     "${CMAKE_CURRENT_BINARY_DIR}/generated")
