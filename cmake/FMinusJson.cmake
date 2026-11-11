# cmake/FMinusJson.cmake: yyjson, the JSON parser behind FMinus's json_* builtins (src/runtime/Json.cpp). Only when
# FMINUS_JSON is on: with it off, nothing is fetched, and there is no target. Its one C file, yyjson.c, builds the
# object library fminus_yyjson, whose objects src/CMakeLists.txt puts into the runtime library: libfminus_runtime.a
# holds the parser, and a host or an executable links nothing more for it.
#
# The source is fetched once (pinned in cmake/FMinusPins.cmake) into ${FMINUS_DEPS_DIR}/src/yyjson-<version>, marked
# with its hash, and every later configure of any build tree uses it there. Only yyjson.c is compiled: yyjson's own
# CMakeLists.txt (its tests, fuzzers, docs and install rules) is never added.
#
# yyjson keeps its own flags, FMinus's warnings and sanitizers stay out of it, and its symbols are hidden: its header
# marks its functions visibility("default") unless yyjson_api is defined, so it is defined empty here, and the C
# visibility preset hides them, as it hides PFFFT's. What FMinus does not use is compiled out
# (FMINUS_YYJSON_DEFINITIONS, which Json.cpp reads the header with too, so both see the same declarations): the writer,
# the incremental reader, files, JSON Pointer and Patch (FMinus prepares its own paths), and every extension of JSON
# (comments, trailing commas, inf and nan, JSON5): what it reads is RFC 8259's JSON, its UTF-8 checked. A depth limit is
# compiled in, FMINUS_JSON_MAX_DEPTH (cmake/FMinusOptions.cmake): a document whose arrays and objects nest deeper,
# counted from the outermost, is not JSON to it. yyjson's reader does not recurse in any case. Its own conversion of
# numbers is compiled out too (YYJSON_DISABLE_FAST_FP_CONV): FMinus reads every number as its text
# (YYJSON_READ_NUMBER_AS_RAW) and converts it with fast_float when a builtin reads it, so the C library's strtod, which
# yyjson would fall back to, is never called.
#
# Gives fminus_yyjson, FMINUS_YYJSON_INCLUDE_DIR (#include <yyjson.h>) and FMINUS_YYJSON_DEFINITIONS.
if(NOT FMINUS_JSON)
  return()
endif()

include(FetchContent)

set(FMINUS_YYJSON_SOURCE_DIR "${FMINUS_DEPS_DIR}/src/yyjson-${FMINUS_YYJSON_VERSION}")
file(LOCK "${FMINUS_YYJSON_SOURCE_DIR}.lock" GUARD FILE)
if(EXISTS "${FMINUS_YYJSON_SOURCE_DIR}/.fminus-populated")
  file(READ "${FMINUS_YYJSON_SOURCE_DIR}/.fminus-populated" _fminus_yyjson_marker)
  if(_fminus_yyjson_marker STREQUAL FMINUS_YYJSON_SHA256)
    set(FETCHCONTENT_SOURCE_DIR_FMINUS_YYJSON "${FMINUS_YYJSON_SOURCE_DIR}")
  endif()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FMINUS_YYJSON)
  message(STATUS "FMinus: fetching yyjson ${FMINUS_YYJSON_VERSION} into ${FMINUS_YYJSON_SOURCE_DIR}")
endif()
FetchContent_Declare(fminus_yyjson
  URL "${FMINUS_YYJSON_URL}"
  URL_HASH SHA256=${FMINUS_YYJSON_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FMINUS_YYJSON_SOURCE_DIR}"
  SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of yyjson's CMake
FetchContent_MakeAvailable(fminus_yyjson)
file(WRITE "${FMINUS_YYJSON_SOURCE_DIR}/.fminus-populated" "${FMINUS_YYJSON_SHA256}")

set(FMINUS_YYJSON_INCLUDE_DIR "${FMINUS_YYJSON_SOURCE_DIR}/src")
set(FMINUS_YYJSON_DEFINITIONS
  yyjson_api=
  YYJSON_DISABLE_WRITER=1
  YYJSON_DISABLE_INCR_READER=1
  YYJSON_DISABLE_FILE=1
  YYJSON_DISABLE_UTILS=1
  YYJSON_DISABLE_FAST_FP_CONV=1
  YYJSON_DISABLE_NON_STANDARD=1
  YYJSON_READER_DEPTH_LIMIT=${FMINUS_JSON_MAX_DEPTH})
add_library(fminus_yyjson OBJECT "${FMINUS_YYJSON_SOURCE_DIR}/src/yyjson.c")
target_include_directories(fminus_yyjson PRIVATE "${FMINUS_YYJSON_INCLUDE_DIR}")
target_compile_definitions(fminus_yyjson PRIVATE ${FMINUS_YYJSON_DEFINITIONS})
set_target_properties(fminus_yyjson PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
