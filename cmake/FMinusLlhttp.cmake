# cmake/FMinusLlhttp.cmake: llhttp, Node.js's HTTP parser, which reads the head and the framing of the requests the web
# library's server receives (src/web/Server.cpp). Only when FMINUS_WEB_SERVER is on: with it off, nothing is fetched,
# and there is no target. Its three C files, the generated parser and its API (src/llhttp.c, api.c, http.c, as the
# release's tag has them), build the object library fminus_llhttp, whose objects src/CMakeLists.txt puts into the web
# library: libfminus_web.a holds the parser, and a host or an executable links nothing more for it.
#
# The source is fetched once (pinned in cmake/FMinusPins.cmake) into ${FMINUS_DEPS_DIR}/src/llhttp-<version>, marked
# with its hash, and every later configure of any build tree uses it there. Only the three files are compiled: llhttp's
# own CMakeLists.txt (which wants a shared library by default, and install rules) is never added.
#
# llhttp keeps its own flags, FMinus's warnings and sanitizers stay out of it, and its symbols are hidden (its header
# marks its functions with nothing on macOS and Linux, so the C visibility preset hides them, as it hides PFFFT's and
# yyjson's; on Windows it marks them __declspec(dllexport), which a static library only acts on in a DLL that links it).
# It allocates nothing and does no I/O: the server feeds it bytes, and counts every limit itself, in its own callbacks.
# Every lenient flag stays off (the parser's default), and the server never sets one.
#
# Gives fminus_llhttp and FMINUS_LLHTTP_INCLUDE_DIR (#include <llhttp.h>).
if(NOT FMINUS_WEB_SERVER)
  return()
endif()

include(FetchContent)

set(FMINUS_LLHTTP_SOURCE_DIR "${FMINUS_DEPS_DIR}/src/llhttp-${FMINUS_LLHTTP_VERSION}")
file(LOCK "${FMINUS_LLHTTP_SOURCE_DIR}.lock" GUARD FILE)
if(EXISTS "${FMINUS_LLHTTP_SOURCE_DIR}/.fminus-populated")
  file(READ "${FMINUS_LLHTTP_SOURCE_DIR}/.fminus-populated" _fminus_llhttp_marker)
  if(_fminus_llhttp_marker STREQUAL FMINUS_LLHTTP_SHA256)
    set(FETCHCONTENT_SOURCE_DIR_FMINUS_LLHTTP "${FMINUS_LLHTTP_SOURCE_DIR}")
  endif()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FMINUS_LLHTTP)
  message(STATUS "FMinus: fetching llhttp ${FMINUS_LLHTTP_VERSION} into ${FMINUS_LLHTTP_SOURCE_DIR}")
endif()
FetchContent_Declare(fminus_llhttp
  URL "${FMINUS_LLHTTP_URL}"
  URL_HASH SHA256=${FMINUS_LLHTTP_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FMINUS_LLHTTP_SOURCE_DIR}"
  SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of llhttp's CMake
FetchContent_MakeAvailable(fminus_llhttp)
file(WRITE "${FMINUS_LLHTTP_SOURCE_DIR}/.fminus-populated" "${FMINUS_LLHTTP_SHA256}")

set(FMINUS_LLHTTP_INCLUDE_DIR "${FMINUS_LLHTTP_SOURCE_DIR}/include")
add_library(fminus_llhttp OBJECT
  "${FMINUS_LLHTTP_SOURCE_DIR}/src/llhttp.c"
  "${FMINUS_LLHTTP_SOURCE_DIR}/src/api.c"
  "${FMINUS_LLHTTP_SOURCE_DIR}/src/http.c")
target_include_directories(fminus_llhttp PRIVATE "${FMINUS_LLHTTP_INCLUDE_DIR}")
set_target_properties(fminus_llhttp PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
message(STATUS "FMinus: the web server: llhttp ${FMINUS_LLHTTP_VERSION} (${FMINUS_LLHTTP_SOURCE_DIR})")
