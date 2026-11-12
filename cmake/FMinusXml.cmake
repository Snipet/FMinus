# cmake/FMinusXml.cmake: pugixml, the XML parser behind FMinus's xml_* builtins (src/runtime/Xml.cpp). Only when
# FMINUS_XML is on: with it off, nothing is fetched, and there is no target. Its one C++ file, pugixml.cpp, builds the
# object library fminus_pugixml, whose objects src/CMakeLists.txt puts into the runtime library: libfminus_runtime.a
# holds the parser, and a host or an executable links nothing more for it.
#
# The source is fetched once (pinned in cmake/FMinusPins.cmake) into ${FMINUS_DEPS_DIR}/src/pugixml-<version>, marked
# with its hash, and every later configure of any build tree uses it there. Only pugixml.cpp is compiled: pugixml's own
# CMakeLists.txt (its tests, install rules and packaging) is never added.
#
# pugixml keeps its own flags (its own C++ standard too: it is written for any), FMinus's warnings and sanitizers stay
# out of it, and its symbols are hidden: the C++ visibility preset hides the functions it defines, inline ones too, as
# PFFFT's are hidden. What FMinus does not use is compiled out (FMINUS_PUGIXML_DEFINITIONS, which Xml.cpp reads the
# header with too, so both see the same declarations): XPath (FMinus prepares its own paths), exceptions (a parse that
# fails, out of memory too, is a status), and the STL (no std::string or iostream in its interface). It reads a DOCTYPE
# only to skip it, and expands only XML's five predefined references and character references: an entity a document
# declares is never expanded and an external one never fetched, so neither "billion laughs" nor an external entity
# reaches anything. It has no limits of its own; Xml.cpp checks FMinus's (FMINUS_XML_MAX_BYTES and _DEPTH,
# cmake/FMinusOptions.cmake). Its own conversion of numbers (the C library's strtod, which reads the locale) is never
# called: Xml.cpp gives the text to fast_float.
#
# Gives fminus_pugixml, FMINUS_PUGIXML_INCLUDE_DIR (#include <pugixml.hpp>) and FMINUS_PUGIXML_DEFINITIONS.
if(NOT FMINUS_XML)
  return()
endif()

include(FetchContent)

set(FMINUS_PUGIXML_SOURCE_DIR "${FMINUS_DEPS_DIR}/src/pugixml-${FMINUS_PUGIXML_VERSION}")
file(LOCK "${FMINUS_PUGIXML_SOURCE_DIR}.lock" GUARD FILE)
if(EXISTS "${FMINUS_PUGIXML_SOURCE_DIR}/.fminus-populated")
  file(READ "${FMINUS_PUGIXML_SOURCE_DIR}/.fminus-populated" _fminus_pugixml_marker)
  if(_fminus_pugixml_marker STREQUAL FMINUS_PUGIXML_SHA256)
    set(FETCHCONTENT_SOURCE_DIR_FMINUS_PUGIXML "${FMINUS_PUGIXML_SOURCE_DIR}")
  endif()
endif()
if(NOT FETCHCONTENT_SOURCE_DIR_FMINUS_PUGIXML)
  message(STATUS "FMinus: fetching pugixml ${FMINUS_PUGIXML_VERSION} into ${FMINUS_PUGIXML_SOURCE_DIR}")
endif()
FetchContent_Declare(fminus_pugixml
  URL "${FMINUS_PUGIXML_URL}"
  URL_HASH SHA256=${FMINUS_PUGIXML_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FMINUS_PUGIXML_SOURCE_DIR}"
  SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of pugixml's CMake
FetchContent_MakeAvailable(fminus_pugixml)
file(WRITE "${FMINUS_PUGIXML_SOURCE_DIR}/.fminus-populated" "${FMINUS_PUGIXML_SHA256}")

set(FMINUS_PUGIXML_INCLUDE_DIR "${FMINUS_PUGIXML_SOURCE_DIR}/src")
set(FMINUS_PUGIXML_DEFINITIONS PUGIXML_NO_XPATH PUGIXML_NO_EXCEPTIONS PUGIXML_NO_STL)
add_library(fminus_pugixml OBJECT "${FMINUS_PUGIXML_SOURCE_DIR}/src/pugixml.cpp")
target_include_directories(fminus_pugixml PRIVATE "${FMINUS_PUGIXML_INCLUDE_DIR}")
target_compile_definitions(fminus_pugixml PRIVATE ${FMINUS_PUGIXML_DEFINITIONS})
set_target_properties(fminus_pugixml PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
