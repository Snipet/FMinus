# cmake/FMinusParserTools.cmake: GNU Bison and Flex, and fminus_add_grammar(). Two modes (FMINUS_PARSER_TOOLS_MODE):
#
#   fetch   (default)  FetchContent fetches the pinned tarballs. The first configure builds each tool (./configure &&
#                      make, about a minute) into ${FMINUS_DEPS_DIR}/install/<tool>-<version>; later configures of
#                      any build tree find the install.
#   system             Bison >= 3.8 and Flex >= 2.6 from PATH, or Homebrew's keg-only /opt/homebrew/opt/bison/bin.
#                      macOS's own /usr/bin/bison is 2.3, too old for the C++ variant skeleton.
#
# Gives FMINUS_BISON_EXECUTABLE and FMINUS_FLEX_EXECUTABLE. It also sets BISON_EXECUTABLE and FLEX_EXECUTABLE, so
# CMake's own FindBISON and FindFLEX (BISON_TARGET, FLEX_TARGET) find the same tools. FLEX_INCLUDE_DIR holds the
# matching FlexLexer.h, for `%option c++` scanners. And it gives:
#
#   fminus_add_grammar(<target>
#       [PARSER <grammar.y>]           Bison  -> <stem>.tab.cpp, <stem>.tab.hpp (+ location.hh), report <stem>.output
#       [LEXERS <scanner.l>...]        Flex   -> <stem>.yy.cpp, <stem>.yy.hpp
#       [BASE_DIR <dir>]               default ${CMAKE_CURRENT_SOURCE_DIR}
#       [OUTPUT_DIR <dir>]             default ${CMAKE_CURRENT_BINARY_DIR}/generated
#       [SCOPE PUBLIC|PRIVATE]         of the include directory OUTPUT_DIR (default PUBLIC)
#       [BISON_FLAGS <flag>...] [FLEX_FLAGS <flag>...])
#
# Generated files keep their path relative to BASE_DIR under OUTPUT_DIR. For example, src/frontend/Parser.y becomes
# generated/frontend/Parser.tab.hpp, so `#include "frontend/Parser.tab.hpp"` works with OUTPUT_DIR on the include
# path. The generated sources join <target>.
include(FetchContent)

set(FMINUS_PARSER_TOOLS_MODE "fetch" CACHE STRING "Where Bison and Flex come from: fetch or system")
set_property(CACHE FMINUS_PARSER_TOOLS_MODE PROPERTY STRINGS fetch system)
set(FMINUS_BISON_FLAGS -Wall -Wcounterexamples CACHE STRING "Flags fminus_add_grammar passes to every Bison run")
set(FMINUS_FLEX_FLAGS "" CACHE STRING "Flags fminus_add_grammar passes to every Flex run")

find_program(FMINUS_MAKE NAMES gmake make)

# Fetches <name>-<version>, builds it out of tree with ./configure <args> and installs it under the deps cache.
function(_fminus_build_autotool name version sha256)
  cmake_parse_arguments(PARSE_ARGV 3 _a "" "" "URLS;CONFIGURE_ARGS")
  set(_prefix "${FMINUS_DEPS_DIR}/install/${name}-${version}")
  set(_stamp "${_prefix}/.fminus-installed")
  if(EXISTS "${_stamp}")
    return()
  endif()
  file(LOCK "${FMINUS_DEPS_DIR}/install/${name}-${version}.lock" GUARD FUNCTION)
  if(EXISTS "${_stamp}")
    return()
  endif()

  message(STATUS "FMinus: fetching and building ${name} ${version} into ${_prefix}")
  FetchContent_Declare(fminus_${name}
    URL ${_a_URLS}
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE   # autotools trees rely on the archive's times (else make wants autoconf)
    SOURCE_DIR "${FMINUS_DEPS_DIR}/src/${name}-${version}")
  FetchContent_MakeAvailable(fminus_${name})   # no CMakeLists.txt in the tarball: populate only

  set(_bdir "${FMINUS_DEPS_DIR}/build/${name}-${version}")
  file(REMOVE_RECURSE "${_bdir}")
  file(MAKE_DIRECTORY "${_bdir}")
  # A host tool: the environment's flags (and this tree's sanitizers) stay out of it.
  set(_env "${CMAKE_COMMAND}" -E env --unset=CFLAGS --unset=CPPFLAGS --unset=CXXFLAGS --unset=LDFLAGS
           "CC=${CMAKE_C_COMPILER}")
  foreach(_step configure build install)
    if(_step STREQUAL "configure")
      set(_cmd ${_env} "${FMINUS_DEPS_DIR}/src/${name}-${version}/configure" "--prefix=${_prefix}" ${_a_CONFIGURE_ARGS})
      set(_dir "${_bdir}")
    elseif(_step STREQUAL "build")
      set(_cmd ${_env} "${FMINUS_MAKE}" -j${FMINUS_DEPS_JOBS})
      set(_dir "${_bdir}")
    else()
      set(_cmd ${_env} "${FMINUS_MAKE}" install)
      set(_dir "${_bdir}")
    endif()
    execute_process(COMMAND ${_cmd} WORKING_DIRECTORY "${_dir}" RESULT_VARIABLE _rc
                    OUTPUT_FILE "${_bdir}/fminus-${_step}.log" ERROR_FILE "${_bdir}/fminus-${_step}.log")
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "FMinus: ${name} ${version}: ${_step} failed (${_rc}); log: ${_bdir}/fminus-${_step}.log")
    endif()
  endforeach()
  file(WRITE "${_stamp}" "${sha256}")
endfunction()

if(FMINUS_PARSER_TOOLS_MODE STREQUAL "fetch")
  if(NOT FMINUS_MAKE)
    message(FATAL_ERROR "FMinus: building Bison and Flex needs make (Xcode's command line tools)")
  endif()
  _fminus_build_autotool(bison "${FMINUS_BISON_VERSION}" "${FMINUS_BISON_SHA256}"
    URLS ${FMINUS_BISON_URLS}
    CONFIGURE_ARGS --disable-nls --enable-relocatable --disable-yacc --disable-dependency-tracking)
  _fminus_build_autotool(flex "${FMINUS_FLEX_VERSION}" "${FMINUS_FLEX_SHA256}"
    URLS ${FMINUS_FLEX_URL}
    CONFIGURE_ARGS --disable-nls --disable-shared --disable-bootstrap --disable-dependency-tracking)
  set(FMINUS_BISON_EXECUTABLE "${FMINUS_DEPS_DIR}/install/bison-${FMINUS_BISON_VERSION}/bin/bison")
  set(FMINUS_FLEX_EXECUTABLE "${FMINUS_DEPS_DIR}/install/flex-${FMINUS_FLEX_VERSION}/bin/flex")
  set(BISON_EXECUTABLE "${FMINUS_BISON_EXECUTABLE}" CACHE FILEPATH "GNU Bison (FMinus's deps cache)" FORCE)
  set(FLEX_EXECUTABLE "${FMINUS_FLEX_EXECUTABLE}" CACHE FILEPATH "Flex (FMinus's deps cache)" FORCE)
  set(FLEX_INCLUDE_DIR "${FMINUS_DEPS_DIR}/install/flex-${FMINUS_FLEX_VERSION}/include" CACHE PATH
      "FlexLexer.h (FMinus's deps cache)" FORCE)
elseif(FMINUS_PARSER_TOOLS_MODE STREQUAL "system")
  find_program(BISON_EXECUTABLE bison HINTS /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin)
  find_program(FLEX_EXECUTABLE flex HINTS /opt/homebrew/opt/flex/bin /usr/local/opt/flex/bin)
else()
  message(FATAL_ERROR "FMinus: FMINUS_PARSER_TOOLS_MODE is '${FMINUS_PARSER_TOOLS_MODE}'; use fetch or system")
endif()

find_package(BISON 3.8 REQUIRED)
find_package(FLEX 2.6 REQUIRED)
set(FMINUS_BISON_EXECUTABLE "${BISON_EXECUTABLE}")
set(FMINUS_FLEX_EXECUTABLE "${FLEX_EXECUTABLE}")
message(STATUS "FMinus: Bison ${BISON_VERSION} (${FMINUS_BISON_EXECUTABLE}), Flex ${FLEX_VERSION} "
               "(${FMINUS_FLEX_EXECUTABLE})")

# The one warning Bison 3.8.2's lalr1.cc skeleton itself raises under fminus_target_defaults: yynerrs_, set and never
# read. Everything else in the generated files (the actions of a grammar or scanner included) gets the full warnings;
# Flex 2.6.4's reentrant output needs no exemption.
set(FMINUS_BISON_OUTPUT_OPTIONS -Wno-unused-but-set-variable)

function(fminus_add_grammar target)
  cmake_parse_arguments(PARSE_ARGV 1 _g "" "PARSER;BASE_DIR;OUTPUT_DIR;SCOPE" "LEXERS;BISON_FLAGS;FLEX_FLAGS")
  if(_g_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "fminus_add_grammar(${target}): unexpected arguments ${_g_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT _g_PARSER AND NOT _g_LEXERS)
    message(FATAL_ERROR "fminus_add_grammar(${target}): give a PARSER, LEXERS or both")
  endif()
  if(NOT _g_BASE_DIR)
    set(_g_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(NOT _g_OUTPUT_DIR)
    set(_g_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  endif()
  if(NOT _g_SCOPE)
    set(_g_SCOPE PUBLIC)
  endif()

  set(_generated "")
  if(_g_PARSER)
    cmake_path(ABSOLUTE_PATH _g_PARSER BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _y)
    cmake_path(RELATIVE_PATH _y BASE_DIRECTORY "${_g_BASE_DIR}" OUTPUT_VARIABLE _rel)
    cmake_path(GET _rel PARENT_PATH _reldir)
    cmake_path(GET _y STEM LAST_ONLY _stem)
    set(_dir "${_g_OUTPUT_DIR}/${_reldir}")
    file(MAKE_DIRECTORY "${_dir}")
    # Run from the output directory with relative names, so the parser includes its header as "<stem>.tab.hpp" and
    # location.hh lands beside it.
    add_custom_command(
      OUTPUT "${_dir}/${_stem}.tab.cpp" "${_dir}/${_stem}.tab.hpp"
      BYPRODUCTS "${_dir}/${_stem}.output"
      COMMAND "${FMINUS_BISON_EXECUTABLE}" ${FMINUS_BISON_FLAGS} ${_g_BISON_FLAGS}
              "--output=${_stem}.tab.cpp" "--header=${_stem}.tab.hpp"
              --report=all "--report-file=${_stem}.output" "${_y}"
      DEPENDS "${_y}"
      WORKING_DIRECTORY "${_dir}"
      COMMENT "Bison ${_rel}"
      VERBATIM)
    set_source_files_properties("${_dir}/${_stem}.tab.cpp" PROPERTIES COMPILE_OPTIONS "${FMINUS_BISON_OUTPUT_OPTIONS}")
    list(APPEND _generated "${_dir}/${_stem}.tab.cpp" "${_dir}/${_stem}.tab.hpp")
  endif()

  foreach(_l IN LISTS _g_LEXERS)
    cmake_path(ABSOLUTE_PATH _l BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _l)
    cmake_path(RELATIVE_PATH _l BASE_DIRECTORY "${_g_BASE_DIR}" OUTPUT_VARIABLE _rel)
    cmake_path(GET _rel PARENT_PATH _reldir)
    cmake_path(GET _l STEM LAST_ONLY _stem)
    set(_dir "${_g_OUTPUT_DIR}/${_reldir}")
    file(MAKE_DIRECTORY "${_dir}")
    add_custom_command(
      OUTPUT "${_dir}/${_stem}.yy.cpp" "${_dir}/${_stem}.yy.hpp"
      COMMAND "${FMINUS_FLEX_EXECUTABLE}" ${FMINUS_FLEX_FLAGS} ${_g_FLEX_FLAGS}
              "--outfile=${_stem}.yy.cpp" "--header-file=${_stem}.yy.hpp" "${_l}"
      DEPENDS "${_l}"
      WORKING_DIRECTORY "${_dir}"
      COMMENT "Flex ${_rel}"
      VERBATIM)
    list(APPEND _generated "${_dir}/${_stem}.yy.cpp" "${_dir}/${_stem}.yy.hpp")
  endforeach()

  set_source_files_properties(${_generated} PROPERTIES SKIP_LINTING ON)
  target_sources(${target} PRIVATE ${_generated})
  target_include_directories(${target} ${_g_SCOPE} "${_g_OUTPUT_DIR}")
endfunction()
