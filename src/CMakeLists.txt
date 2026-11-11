# src/CMakeLists.txt: the fminus library (target fminus_lib, libfminus.a, FMinus::FMinus), the FMinus runtime
# (FMinus::Runtime), and FMinus::Internal for this tree's tools and tests. (The library's target is fminus_lib so the
# driver, tools/fminus/, can be the target fminus.)
#
# fminus_runtime: every .cpp under src/runtime/, PFFFT's objects (the FFT), yyjson's (the JSON parser) when
# FMINUS_JSON is on, and pugixml's (the XML parser) when FMINUS_XML is on. It holds the C functions generated code calls, with no LLVM in it. It reads the generated
# fminus/Config.h (which build options it has), and fast_float's headers when FMINUS_JSON or FMINUS_XML is on. A process that JITs links it (through fminus), and executables from
# support/Aot.h link libfminus_runtime.a itself.
#
# fminus_circuit: every .cpp under src/circuit/, the circuit compiler (include/fminus/Circuit.h: SPICE-style netlists to
# FMinus source). It has no LLVM in it, as the runtime has none, and no grammar (src/circuit/ holds no *.y or *.l: a
# grammar there would be generated into fminus_lib). fminus_lib links it, so FMinus::FMinus's hosts and FMinus::Internal's
# users get it; a host that never calls its reference or analysis drops those objects at link time. It compiles with
# -ffp-contract=off, as the runtime does, so the generator's double arithmetic, and with it the FMinus text it prints, is
# the same on arm64 and x86-64.
#
# fminus_web: every .cpp under src/web/, the web library (web/Web.h: an HTTP(S) client on libcurl, one I/O thread for
# the process, and the host's policy over what a program may reach), built only when FMINUS_WEB is on, and linked by
# fminus_lib only then. No LLVM in it, as the runtime has none; libcurl and its TLS library are linked privately
# (cmake/FMinusWeb.cmake), and only fminus_web's own C functions and its C++ API are what FMinus calls. With the option
# off its sources are no one's: nothing under src/web/ is compiled. Its server (web/Server.h) is part of it with
# FMINUS_WEB_SERVER on, llhttp's objects inside it and fast_float's headers read by one file; with that option off the
# server's sources compile to nothing (each is guarded by fminus/Config.h's FMINUS_HAS_WEB_SERVER).
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
set(_fminus_circuit_sources ${_fminus_all_sources})
list(FILTER _fminus_circuit_sources INCLUDE REGEX "^circuit/")
set(_fminus_web_sources ${_fminus_all_sources})
list(FILTER _fminus_web_sources INCLUDE REGEX "^web/")
set(_fminus_sources ${_fminus_all_sources})
list(FILTER _fminus_sources EXCLUDE REGEX "^runtime/")
list(FILTER _fminus_sources EXCLUDE REGEX "^circuit/")
list(FILTER _fminus_sources EXCLUDE REGEX "^web/")
list(TRANSFORM _fminus_runtime_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM _fminus_circuit_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM _fminus_web_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM _fminus_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
file(GLOB_RECURSE _fminus_public_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/*.h")

# ---- the runtime ----------------------------------------------------------------------------------------------------
add_library(fminus_runtime STATIC ${_fminus_runtime_sources})
add_library(FMinus::Runtime ALIAS fminus_runtime)
fminus_target_defaults(fminus_runtime)
target_include_directories(fminus_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/include")
# The math kernels (runtime/MathKernels.h) fuse a multiply and an add only where they say so, as FMinus's IR does, so
# they compute the same operations on every target. (clang-cl takes clang's spelling through /clang:.)
if(FMINUS_MSVC_FRONTEND)
  target_compile_options(fminus_runtime PRIVATE /clang:-ffp-contract=off)
else()
  target_compile_options(fminus_runtime PRIVATE -ffp-contract=off)
endif()
# The FFT, PFFFT (cmake/FMinusFft.cmake): its objects go into this library, so it holds the FFT itself.
target_sources(fminus_runtime PRIVATE $<TARGET_OBJECTS:fminus_pffft>)
target_include_directories(fminus_runtime SYSTEM PRIVATE "${FMINUS_PFFFT_INCLUDE_DIR}")
target_compile_definitions(fminus_runtime PRIVATE PFFFT_STATIC_DEFINE)
# fast_float (cmake/FMinusFastFloat.cmake), the parsers' numbers (runtime/Numbers.cpp), when FMINUS_JSON or FMINUS_XML.
if(TARGET fminus_fast_float)
  target_link_libraries(fminus_runtime PRIVATE fminus_fast_float)
endif()
# The JSON parser, yyjson (cmake/FMinusJson.cmake), when FMINUS_JSON: its objects go into this library, and
# runtime/Json.cpp reads its header with the definitions it was compiled with.
if(TARGET fminus_yyjson)
  target_sources(fminus_runtime PRIVATE $<TARGET_OBJECTS:fminus_yyjson>)
  target_include_directories(fminus_runtime SYSTEM PRIVATE "${FMINUS_YYJSON_INCLUDE_DIR}")
  target_compile_definitions(fminus_runtime PRIVATE ${FMINUS_YYJSON_DEFINITIONS})
endif()
# The XML parser, pugixml (cmake/FMinusXml.cmake), when FMINUS_XML: the same.
if(TARGET fminus_pugixml)
  target_sources(fminus_runtime PRIVATE $<TARGET_OBJECTS:fminus_pugixml>)
  target_include_directories(fminus_runtime SYSTEM PRIVATE "${FMINUS_PUGIXML_INCLUDE_DIR}")
  target_compile_definitions(fminus_runtime PRIVATE ${FMINUS_PUGIXML_DEFINITIONS})
endif()
# The pool behind parallel for and parallel { } (runtime/Parallel.cpp) makes threads: the system's thread library, on a
# system where it is one of its own.
find_package(Threads REQUIRED)
target_link_libraries(fminus_runtime PUBLIC Threads::Threads)

# ---- the circuit compiler -------------------------------------------------------------------------------------------
add_library(fminus_circuit STATIC ${_fminus_circuit_sources})
add_library(FMinus::Circuit ALIAS fminus_circuit)
fminus_target_defaults(fminus_circuit)
target_include_directories(fminus_circuit PUBLIC "${PROJECT_SOURCE_DIR}/include" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fminus_circuit INTERFACE cxx_std_17)   # what include/fminus/Circuit.h needs
if(FMINUS_MSVC_FRONTEND)
  target_compile_options(fminus_circuit PRIVATE /clang:-ffp-contract=off)
else()
  target_compile_options(fminus_circuit PRIVATE -ffp-contract=off)
endif()

# ---- the web library (FMINUS_WEB) -----------------------------------------------------------------------------------
if(FMINUS_WEB)
  add_library(fminus_web STATIC ${_fminus_web_sources})
  add_library(FMinus::Web ALIAS fminus_web)
  fminus_target_defaults(fminus_web)
  target_include_directories(fminus_web PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/include")
  target_link_libraries(fminus_web PRIVATE fminus_web_curl PUBLIC Threads::Threads)
  if(APPLE)
    # web/RootsMac.cpp: the Keychain's roots, through Security.framework.
    target_link_libraries(fminus_web PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  # The server (FMINUS_WEB_SERVER): llhttp's objects (cmake/FMinusLlhttp.cmake) go into this library, and fast_float
  # (cmake/FMinusFastFloat.cmake) reads a query's numbers.
  if(TARGET fminus_llhttp)
    target_sources(fminus_web PRIVATE $<TARGET_OBJECTS:fminus_llhttp>)
    target_include_directories(fminus_web SYSTEM PRIVATE "${FMINUS_LLHTTP_INCLUDE_DIR}")
    target_link_libraries(fminus_web PRIVATE fminus_fast_float)
  endif()
endif()

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
target_link_libraries(fminus_lib PUBLIC fminus_runtime fminus_circuit PRIVATE FMinus::LLVM)   # LLVM: linked, not included
if(FMINUS_WEB)
  target_link_libraries(fminus_lib PUBLIC fminus_web)
endif()
# What support/Aot.cpp links executables with: this tree's C++ compiler driver, runtime library and sanitizers.
target_compile_definitions(fminus_lib PRIVATE
  "FMINUS_AOT_DRIVER=\"${CMAKE_CXX_COMPILER}\""
  "FMINUS_RUNTIME_LIBRARY=\"$<TARGET_FILE:fminus_runtime>\""
  "FMINUS_AOT_SANITIZE=\"${FMINUS_SANITIZE_ARG}\"")
# And, with the web library, what an executable that calls it links besides: libfminus_web.a, then FMINUS_WEB_LINK
# (cmake/FMinusWeb.cmake), as C strings, one a line, in generated/support/WebLink.inc.
if(FMINUS_WEB)
  set(_fminus_web_link "\"$<TARGET_FILE:fminus_web>\",\n")
  foreach(_item IN LISTS FMINUS_WEB_LINK)
    string(APPEND _fminus_web_link "\"${_item}\",\n")
  endforeach()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/support/WebLink.inc"
       CONTENT "// Generated by src/CMakeLists.txt: what an executable that calls the web library links.\n${_fminus_web_link}")
  target_include_directories(fminus_lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

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
# With the web library on Mbed TLS, its tests make a certificate authority and serve TLS with it (tests/unit/WebTest.cpp).
if(TARGET fminus_web_mbedtls)
  target_link_libraries(fminus_internal INTERFACE fminus_web_mbedtls)
endif()
