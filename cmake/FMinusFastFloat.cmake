# cmake/FMinusFastFloat.cmake: fast_float, the reader of numbers behind the runtime's parsers (src/runtime/Numbers.cpp):
# text to a float correctly rounded, straight from its digits and never through a double, and the same in every
# locale, where C's strtof reads a "0,5" in a German one; and behind the web server's query numbers
# (src/web/Server.cpp). Only when FMINUS_JSON, FMINUS_XML or FMINUS_WEB_SERVER is on: with all three off, nothing is
# fetched, and there is no target.
#
# It is headers only. The release's tag is fetched once (pinned in cmake/FMinusPins.cmake) into
# ${FMINUS_DEPS_DIR}/src/fast_float-<version>, marked with its hash, and every later configure of any build tree uses
# it there. Its own CMakeLists.txt (its tests, benchmarks and install rules) is never added: the interface library
# fminus_fast_float gives its include/ directory, as a system one, so FMinus's warnings stay out of it. Its code is
# compiled where the runtime includes it, in one file (Numbers.cpp), which hides its symbols, and where the web library
# does, in one file of its own too (web/Server.cpp).
#
# Gives fminus_fast_float (#include <fast_float/fast_float.h>).
if(NOT FMINUS_JSON AND NOT FMINUS_XML AND NOT FMINUS_WEB_SERVER)
  return()
endif()

include(FetchContent)

set(FMINUS_FAST_FLOAT_SOURCE_DIR "${FMINUS_DEPS_DIR}/src/fast_float-${FMINUS_FAST_FLOAT_VERSION}")
file(LOCK "${FMINUS_FAST_FLOAT_SOURCE_DIR}.lock" GUARD FILE)
if(EXISTS "${FMINUS_FAST_FLOAT_SOURCE_DIR}/.fminus-populated")
  file(READ "${FMINUS_FAST_FLOAT_SOURCE_DIR}/.fminus-populated" _fminus_fast_float_marker)
  if(_fminus_fast_float_marker STREQUAL FMINUS_FAST_FLOAT_SHA256)
    set(FETCHCONTENT_SOURCE_DIR_FMINUS_FAST_FLOAT "${FMINUS_FAST_FLOAT_SOURCE_DIR}")
  endif()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FMINUS_FAST_FLOAT)
  message(STATUS "FMinus: fetching fast_float ${FMINUS_FAST_FLOAT_VERSION} into ${FMINUS_FAST_FLOAT_SOURCE_DIR}")
endif()
FetchContent_Declare(fminus_fast_float
  URL "${FMINUS_FAST_FLOAT_URL}"
  URL_HASH SHA256=${FMINUS_FAST_FLOAT_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FMINUS_FAST_FLOAT_SOURCE_DIR}"
  SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of fast_float's CMake
FetchContent_MakeAvailable(fminus_fast_float)
file(WRITE "${FMINUS_FAST_FLOAT_SOURCE_DIR}/.fminus-populated" "${FMINUS_FAST_FLOAT_SHA256}")

add_library(fminus_fast_float INTERFACE)
target_include_directories(fminus_fast_float SYSTEM INTERFACE "${FMINUS_FAST_FLOAT_SOURCE_DIR}/include")
