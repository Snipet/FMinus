# cmake/FMinusFft.cmake: PFFFT, the FFT behind FMinus's fft_* builtins (src/runtime/Fft.cpp). Two of its C files,
# pffft.c and pffft_common.c, build the object library fminus_pffft, whose objects src/CMakeLists.txt puts into the
# runtime library: libfminus_runtime.a holds the FFT, and a host or an executable links nothing more for it.
#
# The source is fetched once (pinned in cmake/FMinusPins.cmake) into ${FMINUS_DEPS_DIR}/src/pffft-<commit>, marked
# with its hash, and every later configure of any build tree uses it there. Only those two files are compiled: PFFFT's
# own CMakeLists.txt (its tests, benchmarks and the other FFT libraries it compares with) is never added.
#
# PFFFT keeps its own flags, FMinus's warnings and sanitizers stay out of it, and its symbols are hidden: a plugin that
# links FMinus exports none of them. Its SIMD: NEON on arm64, which PFFFT uses only when asked (PFFFT_ENABLE_NEON; the
# header itself checks that the target is ARM, so the define is safe on any target, and in a universal binary); SSE on
# x86-64, which it finds by itself; scalar code elsewhere.
#
# Gives fminus_pffft and FMINUS_PFFFT_INCLUDE_DIR (#include <pffft/pffft.h>).
include(FetchContent)

set(FMINUS_PFFFT_SOURCE_DIR "${FMINUS_DEPS_DIR}/src/pffft-${FMINUS_PFFFT_COMMIT}")
file(LOCK "${FMINUS_DEPS_DIR}/src/pffft-${FMINUS_PFFFT_COMMIT}.lock" GUARD FILE)
if(EXISTS "${FMINUS_PFFFT_SOURCE_DIR}/.fminus-populated")
  file(READ "${FMINUS_PFFFT_SOURCE_DIR}/.fminus-populated" _fminus_pffft_marker)
  if(_fminus_pffft_marker STREQUAL FMINUS_PFFFT_SHA256)
    set(FETCHCONTENT_SOURCE_DIR_FMINUS_PFFFT "${FMINUS_PFFFT_SOURCE_DIR}")
  endif()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FMINUS_PFFFT)
  message(STATUS "FMinus: fetching PFFFT ${FMINUS_PFFFT_COMMIT} into ${FMINUS_PFFFT_SOURCE_DIR}")
endif()
FetchContent_Declare(fminus_pffft
  URL "${FMINUS_PFFFT_URL}"
  URL_HASH SHA256=${FMINUS_PFFFT_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FMINUS_PFFFT_SOURCE_DIR}"
  SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of PFFFT's CMake
FetchContent_MakeAvailable(fminus_pffft)
file(WRITE "${FMINUS_PFFFT_SOURCE_DIR}/.fminus-populated" "${FMINUS_PFFFT_SHA256}")

set(FMINUS_PFFFT_INCLUDE_DIR "${FMINUS_PFFFT_SOURCE_DIR}/include")
add_library(fminus_pffft OBJECT
  "${FMINUS_PFFFT_SOURCE_DIR}/src/pffft.c" "${FMINUS_PFFFT_SOURCE_DIR}/src/pffft_common.c")
target_include_directories(fminus_pffft PRIVATE "${FMINUS_PFFFT_SOURCE_DIR}/src" "${FMINUS_PFFFT_INCLUDE_DIR}/pffft")
target_compile_definitions(fminus_pffft PRIVATE PFFFT_STATIC_DEFINE PFFFT_ENABLE_NEON=1 _USE_MATH_DEFINES)
set_target_properties(fminus_pffft PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(fminus_pffft PRIVATE -Wno-\#pragma-messages)   # its SIMD headers announce themselves
endif()
