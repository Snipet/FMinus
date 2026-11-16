# packaging/CMakeLists.txt: what `cmake --install <tree>` installs, and the archive `cpack` packs from it
# (docs/building.md, "Installing"). Every install rule of FMinus's compiler is here, so that this directory's own
# install script, <tree>/packaging/cmake_install.cmake, installs all of it: the install test (tests/install/) runs that
# script into a directory of its own, where `cmake --install` would also write install_manifest.txt, the file an
# uninstall reads, into the build tree. CMakeLists.txt adds it with the tools (FMINUS_BUILD_TOOLS): the compiler is
# what it installs.
#
# The layout is GNU's (GNUInstallDirs, CMakeLists.txt), the same on every system, under any prefix:
#
#   bin/fminus                component compiler: the driver, LLVM linked in (bin/fminus.exe on Windows)
#   lib/fminus/               component runtime: libfminus_runtime.a (fminus_runtime.lib on Windows), what fminus links
#                             into every executable it makes; with the web library (FMINUS_WEB) also libfminus_web.a
#                             and libcurl's and Mbed TLS's archives (FMINUS_WEB_ARCHIVES), which an executable that
#                             calls it links. A directory of FMinus's own, as compilers keep theirs (lib/gcc/,
#                             lib/clang/), so that nothing of it lands among the system's libraries. fminus finds it
#                             from its own directory (support/Aot.h), so the prefix can move.
#   share/doc/fminus/         component licenses: FMinus's LICENSE (GPL-3.0), NOTICES.txt (what each of the others is
#                             and which binary holds it), and third-party/<name>/, each dependency's own license file,
#                             from its sources in the deps cache (README.md's "License" says what each asks of a binary
#                             that holds it). A package of the compiler or the runtime carries these.
#   bin/fminus-build,         component devtools, installed only when named (`cmake --install <tree> --component
#   bin/fminus-hostinfo       devtools`): tools for FMinus's own development (hand-written IR to an executable; what
#                             the JIT targets, of which `fminus --version` says what a user needs)
#
# CPack packs the default components (compiler, runtime, licenses) into one archive, fminus-<version>-<os>-<arch>
# (linux, macos or windows; x64 or arm64): a .tar.gz on Linux and macOS, a .zip on Windows, with that name as its top
# directory. `cpack --config <tree>/CPackConfig.cmake` (or `cmake --build <tree> --target package`) writes it into the
# build tree; .github/workflows/release.yml builds them.

# ---- the compiler ---------------------------------------------------------------------------------------------------
install(TARGETS fminus RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT compiler)

# ---- the runtime's archives -----------------------------------------------------------------------------------------
install(TARGETS fminus_runtime ARCHIVE DESTINATION "${FMINUS_INSTALL_RUNTIME_DIR}" COMPONENT runtime)
if(FMINUS_WEB)
  install(TARGETS fminus_web ARCHIVE DESTINATION "${FMINUS_INSTALL_RUNTIME_DIR}" COMPONENT runtime)
  install(FILES ${FMINUS_WEB_ARCHIVES} DESTINATION "${FMINUS_INSTALL_RUNTIME_DIR}" COMPONENT runtime)
endif()

# ---- the development tools, only when named -------------------------------------------------------------------------
foreach(_tool fminus-build fminus-hostinfo)
  if(TARGET ${_tool})
    install(TARGETS ${_tool} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT devtools EXCLUDE_FROM_ALL)
  endif()
endforeach()

# ---- the licenses ---------------------------------------------------------------------------------------------------
# Each dependency whose code an installed file holds: its name, version, license, its license file (in the deps cache's
# source, or the build tree's for CLI11), and what holds it. bin/fminus holds all of them (LLVM, CLI11, the runtime, and
# with FMINUS_WEB the web library); every executable fminus makes holds what the runtime's archive holds (PFFFT, and
# yyjson and pugixml in a build with them), and with the web library, an executable that calls it libcurl's, Mbed
# TLS's and llhttp's code too.
set(_fminus_notices "")
set(_fminus_notice_files "")
function(_fminus_notice name version license holds)
  set(_files "")
  foreach(_file IN LISTS ARGN)
    if(NOT EXISTS "${_file}")
      message(FATAL_ERROR "FMinus: ${name}'s license file ${_file} is missing: the install carries it")
    endif()
    cmake_path(GET _file FILENAME _base)
    list(APPEND _files "third-party/${name}/${_base}")
    install(FILES "${_file}" DESTINATION "${FMINUS_INSTALL_DOC_DIR}/third-party/${name}" COMPONENT licenses)
  endforeach()
  list(JOIN _files ", " _files)
  string(APPEND _fminus_notices "${name} ${version}: ${license}\n    ${_files}\n    in ${holds}\n")
  set(_fminus_notices "${_fminus_notices}" PARENT_SCOPE)
endfunction()

set(_held_by_runtime
    "bin/fminus, ${FMINUS_INSTALL_RUNTIME_DIR}/$<TARGET_FILE_NAME:fminus_runtime>, and every executable fminus makes")
if(FMINUS_LLVM_LICENSE)
  _fminus_notice(llvm "${FMINUS_LLVM_PACKAGE_VERSION}" "Apache License 2.0 with LLVM Exceptions" "bin/fminus"
                 "${FMINUS_LLVM_LICENSE}")
else()
  # A prebuilt LLVM from before its flavor's install held a copy, without the deps cache's source: said at install.
  message(STATUS "FMinus: LLVM's LICENSE.TXT is in neither its install nor the deps cache's source: an install of "
                 "this tree lacks it")
  install(CODE "message(WARNING \"FMinus: this install lacks LLVM's LICENSE.TXT, which bin/fminus, linking LLVM, must "
                                "carry: unpack llvm-project ${FMINUS_LLVM_VERSION}'s llvm/LICENSE.TXT into "
                                "${FMINUS_LLVM_SOURCE_DIR}/llvm/ and configure again\")"
          COMPONENT licenses)
  string(APPEND _fminus_notices "llvm ${FMINUS_LLVM_PACKAGE_VERSION}: Apache License 2.0 with LLVM Exceptions\n    "
                                "(its LICENSE.TXT is missing from this install)\n    in bin/fminus\n")
endif()
_fminus_notice(cli11 "${FMINUS_CLI11_VERSION}" "BSD 3-Clause License" "bin/fminus" "${cli11_SOURCE_DIR}/LICENSE")
_fminus_notice(pffft "${FMINUS_PFFFT_COMMIT}" "FFTPACKv5's BSD-style license" "${_held_by_runtime}"
               "${FMINUS_PFFFT_SOURCE_DIR}/LICENSE.txt")
if(FMINUS_JSON)
  _fminus_notice(yyjson "${FMINUS_YYJSON_VERSION}" "MIT License" "${_held_by_runtime}"
                 "${FMINUS_YYJSON_SOURCE_DIR}/LICENSE")
endif()
if(FMINUS_XML)
  _fminus_notice(pugixml "${FMINUS_PUGIXML_VERSION}" "MIT License" "${_held_by_runtime}"
                 "${FMINUS_PUGIXML_SOURCE_DIR}/LICENSE.md")
endif()
if(TARGET fminus_fast_float)
  _fminus_notice(fast_float "${FMINUS_FAST_FLOAT_VERSION}"
                 "Apache License 2.0, MIT License or Boost Software License 1.0, at the user's choice (the last asks \
no notice of a binary)"
                 "${_held_by_runtime}"
                 "${FMINUS_FAST_FLOAT_SOURCE_DIR}/LICENSE-APACHE" "${FMINUS_FAST_FLOAT_SOURCE_DIR}/LICENSE-MIT"
                 "${FMINUS_FAST_FLOAT_SOURCE_DIR}/LICENSE-BOOST")
endif()
if(FMINUS_WEB)
  set(_held_by_web
      "bin/fminus, ${FMINUS_INSTALL_RUNTIME_DIR}/, and every executable fminus makes of a program that calls the web \
library")
  _fminus_notice(curl "${FMINUS_CURL_VERSION}" "curl License (MIT-like)" "${_held_by_web}"
                 "${FMINUS_CURL_SOURCE_DIR}/COPYING")
  if(FMINUS_MBEDTLS_SOURCE_DIR)
    _fminus_notice(mbedtls "${FMINUS_MBEDTLS_VERSION}"
                   "Apache License 2.0 or GPL-2.0-or-later, at the user's choice" "${_held_by_web}"
                   "${FMINUS_MBEDTLS_SOURCE_DIR}/LICENSE")
  endif()
  if(FMINUS_WEB_SERVER)
    _fminus_notice(llhttp "${FMINUS_LLHTTP_VERSION}" "MIT License" "${_held_by_web}"
                   "${FMINUS_LLHTTP_SOURCE_DIR}/LICENSE")
  endif()
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/NOTICES.txt" CONTENT
"FMinus ${PROJECT_VERSION}: the licenses of what this install holds.

FMinus is free software under the GNU General Public License, version 3 (LICENSE, beside this file).

bin/fminus links, and ${FMINUS_INSTALL_RUNTIME_DIR}/ holds, code of these projects, each under its own license, whose
text is in third-party/<name>/. An executable fminus makes links the runtime's archive, and so holds the code this list
says it does: a distribution of such an executable carries those notices too.

${_fminus_notices}")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/NOTICES.txt"
        DESTINATION "${FMINUS_INSTALL_DOC_DIR}" COMPONENT licenses)

# ---- CPack: one archive of the default components -------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_fminus_arch x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64|aarch64)$")
  set(_fminus_arch arm64)
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _fminus_arch)
endif()
if(APPLE)
  set(_fminus_os macos)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$")   # a build for one architecture names it
    string(REPLACE x86_64 x64 _fminus_arch "${CMAKE_OSX_ARCHITECTURES}")
  endif()
elseif(WIN32)
  set(_fminus_os windows)
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _fminus_os)
endif()
set(CPACK_PACKAGE_NAME fminus)
set(CPACK_PACKAGE_VENDOR FMinus)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "fminus-${PROJECT_VERSION}-${_fminus_os}-${_fminus_arch}")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
set(CPACK_SOURCE_GENERATOR "")   # no source package: the repository is the source
# This directory's install script: the default components, the same files as `cmake --install`, and nothing of another
# directory's (the Python package's, python/, in a tree with it).
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_CURRENT_BINARY_DIR};FMinus;ALL;/")
set(CPACK_ARCHIVE_THREADS 0)     # every core compresses
include(CPack)
