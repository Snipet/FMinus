# tests/install/InstallTest.cmake: the ctest test "install" (tests/CMakeLists.txt), a CMake script. It installs the
# build into a directory of its own, moves the prefix somewhere else, as a user unpacks an archive, and runs the
# installed fminus from there: --version, --print-runtime (the archives it names must be the moved install's, not the
# build tree's: that is what proves the installed compiler does not use the tree it was built in), --run, and
# `fminus note.fm -o note`, whose executable it runs; in a build with the web library, an executable that calls it too,
# linked against the installed archives.
#
# It installs with <tree>/packaging/cmake_install.cmake, the script `cmake --install` runs for every one of FMinus's
# install rules (packaging/CMakeLists.txt), so that it writes nothing into the build tree but its own directory:
# `cmake --install` would write install_manifest.txt into the tree, the file a user's own install is uninstalled by.
#
# Given with -D: FMINUS_BUILD_DIR, FMINUS_WORK_DIR (its directory, made anew), FMINUS_SOURCE_DIR, FMINUS_VERSION,
# FMINUS_CONFIG (the build's configuration), FMINUS_BIN_DIR, FMINUS_RUNTIME_DIR_REL and FMINUS_DOC_DIR (the install's
# directories, relative to its prefix), FMINUS_RUNTIME_NAME (the runtime archive's file name), FMINUS_EXE_SUFFIX and
# FMINUS_WEB (1 in a build with the web library).
cmake_minimum_required(VERSION 3.30)

foreach(_variable FMINUS_BUILD_DIR FMINUS_WORK_DIR FMINUS_SOURCE_DIR FMINUS_VERSION FMINUS_BIN_DIR
                  FMINUS_RUNTIME_DIR_REL FMINUS_DOC_DIR FMINUS_RUNTIME_NAME)
  if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
    message(FATAL_ERROR "InstallTest.cmake: ${_variable} is not given")
  endif()
endforeach()

# A path as the driver prints it (native separators; on Windows, in any case) made comparable with CMake's.
function(_comparable path out)
  file(TO_CMAKE_PATH "${path}" _path)
  if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_path}" _path)
  endif()
  set(${out} "${_path}" PARENT_SCOPE)
endfunction()

# Runs a command with FMINUS_RUNTIME_DIR and FMINUS_LINK_DRIVER unset, unless `ENV` sets them, and stores its status,
# stdout and stderr in <prefix>_STATUS, <prefix>_OUT and <prefix>_ERR.
function(_run prefix)
  cmake_parse_arguments(PARSE_ARGV 1 _a "" "WORKING_DIRECTORY" "ENV;COMMAND")
  if(NOT _a_WORKING_DIRECTORY)
    set(_a_WORKING_DIRECTORY "${FMINUS_WORK_DIR}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=FMINUS_RUNTIME_DIR --unset=FMINUS_LINK_DRIVER ${_a_ENV} ${_a_COMMAND}
    WORKING_DIRECTORY "${_a_WORKING_DIRECTORY}"
    RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  list(JOIN _a_ENV " " _env)
  list(JOIN _a_COMMAND " " _command)
  message(STATUS "$ ${_env} ${_command}\n${_out}${_err}(status ${_status})")
  set(${prefix}_STATUS "${_status}" PARENT_SCOPE)
  set(${prefix}_OUT "${_out}" PARENT_SCOPE)
  set(${prefix}_ERR "${_err}" PARENT_SCOPE)
endfunction()

function(_expect condition message)
  if(NOT (${condition}))
    message(FATAL_ERROR "install test: ${message}")
  endif()
endfunction()

# ---- install into a directory of the test's own --------------------------------------------------------------------
file(REMOVE_RECURSE "${FMINUS_WORK_DIR}")
file(MAKE_DIRECTORY "${FMINUS_WORK_DIR}")
set(_prefix "${FMINUS_WORK_DIR}/prefix")
set(_install "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${_prefix}")
if(FMINUS_CONFIG)
  list(APPEND _install "-DCMAKE_INSTALL_CONFIG_NAME=${FMINUS_CONFIG}")
endif()
_run(INSTALL COMMAND ${_install} -P "${FMINUS_BUILD_DIR}/packaging/cmake_install.cmake")
if(NOT INSTALL_STATUS EQUAL 0)
  message(FATAL_ERROR "install test: installing failed (${INSTALL_STATUS})")
endif()

set(_fminus "${FMINUS_BIN_DIR}/fminus${FMINUS_EXE_SUFFIX}")
foreach(_file "${_fminus}" "${FMINUS_RUNTIME_DIR_REL}/${FMINUS_RUNTIME_NAME}" "${FMINUS_DOC_DIR}/LICENSE"
              "${FMINUS_DOC_DIR}/NOTICES.txt" "${FMINUS_DOC_DIR}/third-party/llvm/LICENSE.TXT"
              "${FMINUS_DOC_DIR}/third-party/cli11/LICENSE" "${FMINUS_DOC_DIR}/third-party/pffft/LICENSE.txt")
  _expect("EXISTS;${_prefix}/${_file}" "the install has no ${_file}")
endforeach()
if(FMINUS_WEB)
  _expect("EXISTS;${_prefix}/${FMINUS_DOC_DIR}/third-party/curl/COPYING" "the install has no notice of libcurl's")
endif()
# The development tools are a component of their own, installed only when it is named.
foreach(_tool fminus-build fminus-hostinfo)
  _expect("NOT;EXISTS;${_prefix}/${FMINUS_BIN_DIR}/${_tool}${FMINUS_EXE_SUFFIX}"
          "${_tool} is installed by default (it is the component devtools)")
endforeach()

# ---- move the prefix, as an archive is unpacked anywhere -------------------------------------------------------------
set(_moved "${FMINUS_WORK_DIR}/elsewhere/fminus-${FMINUS_VERSION}")
file(MAKE_DIRECTORY "${FMINUS_WORK_DIR}/elsewhere")
file(RENAME "${_prefix}" "${_moved}")
file(REAL_PATH "${_moved}" _moved)
set(_fminus "${_moved}/${_fminus}")

# ---- --version ------------------------------------------------------------------------------------------------------
_run(VERSION COMMAND "${_fminus}" --version)
_expect("VERSION_STATUS;EQUAL;0" "--version failed")
string(REGEX MATCH "^fminus version ([0-9.]+) \\(LLVM [0-9.]+[^)]*\\)\nTarget: [^\n]+\nFeatures: [-+]json [-+]xml [-+]web [-+]web-server [-+]files [-+]files-write [-+]files-anywhere\nInstalledDir: ([^\n]+)\n$" _matched
       "${VERSION_OUT}")
_expect("_matched" "--version does not say what it should")
set(_installed_dir "${CMAKE_MATCH_2}")
_expect("CMAKE_MATCH_1;STREQUAL;${FMINUS_VERSION}" "--version says ${CMAKE_MATCH_1}, not ${FMINUS_VERSION}")
_comparable("${_installed_dir}" _installed_dir)
_comparable("${_moved}/${FMINUS_BIN_DIR}" _expected_dir)
_expect("_installed_dir;STREQUAL;${_expected_dir}" "--version's InstalledDir is ${_installed_dir}, not ${_expected_dir}")
if(FMINUS_WEB)
  _expect("VERSION_OUT;MATCHES;\\+web " "--version of a build with the web library says -web")
endif()

# ---- --print-runtime: the moved install's archives, not the build tree's --------------------------------------------
_run(PRINT COMMAND "${_fminus}" --print-runtime)
_expect("PRINT_STATUS;EQUAL;0" "--print-runtime failed")
_expect("PRINT_OUT;MATCHES;(^|\n)runtime: ([^\n]+)\n" "--print-runtime names no runtime")
string(REGEX MATCH "(^|\n)runtime: ([^\n]+)\n" _matched "${PRINT_OUT}")
_comparable("${CMAKE_MATCH_2}" _runtime)
_comparable("${_moved}/${FMINUS_RUNTIME_DIR_REL}/${FMINUS_RUNTIME_NAME}" _expected_runtime)
_expect("_runtime;STREQUAL;${_expected_runtime}"
        "the installed fminus links ${_runtime}, not the moved install's ${_expected_runtime}")
_expect("PRINT_OUT;MATCHES;\nruntime-found: installed" "--print-runtime does not say the runtime is the install's")
_expect("PRINT_OUT;MATCHES;\ndriver: [^\n]+\ndriver-found: " "--print-runtime names no driver")
if(FMINUS_WEB)
  # Every archive the web library links is the moved install's: none is the build tree's or the deps cache's.
  string(REGEX MATCH "\nweb: ([^\n]+)\n" _matched "${PRINT_OUT}")
  _expect("_matched" "--print-runtime of a build with the web library names no web archives")
  separate_arguments(_items NATIVE_COMMAND "${CMAKE_MATCH_1}")
  _comparable("${_moved}/${FMINUS_RUNTIME_DIR_REL}/" _expected_prefix)
  set(_archives 0)
  foreach(_item IN LISTS _items)
    _comparable("${_item}" _item)
    if(_item MATCHES "/")
      string(FIND "${_item}" "${_expected_prefix}" _at)
      _expect("_at;EQUAL;0" "the installed fminus links the web library's ${_item}, outside the moved install")
      math(EXPR _archives "${_archives} + 1")
    endif()
  endforeach()
  _expect("_archives;GREATER_EQUAL;2" "--print-runtime names ${_archives} web archives")
endif()

# ---- --run, and an executable made and run ---------------------------------------------------------------------------
set(_note "${FMINUS_SOURCE_DIR}/examples/note/note.fm")
_run(RUN COMMAND "${_fminus}" "${_note}" --run main 69)
_expect("RUN_STATUS;EQUAL;0" "--run failed")
_expect("RUN_OUT;STREQUAL;440\n" "--run main 69 printed '${RUN_OUT}', not 440")

_run(LINK COMMAND "${_fminus}" "${_note}" -o note)
_expect("LINK_STATUS;EQUAL;0" "fminus note.fm -o note failed")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  # The object is the system linker's to take as it is (support/HostTarget.h): no warning, of text relocations or
  # anything else.
  if(NOT LINK_ERR STREQUAL "")
    message(FATAL_ERROR "install test: linking printed: ${LINK_ERR}")
  endif()
endif()
_run(NOTE COMMAND "${FMINUS_WORK_DIR}/note${FMINUS_EXE_SUFFIX}" 69)
_expect("NOTE_STATUS;EQUAL;0" "the executable failed")
_expect("NOTE_OUT;STREQUAL;440\n" "the executable printed '${NOTE_OUT}', not 440")

if(FMINUS_WEB)
  _run(WEB COMMAND "${_fminus}" "${FMINUS_SOURCE_DIR}/tests/install/refused.fm" -o refused)
  _expect("WEB_STATUS;EQUAL;0" "fminus refused.fm -o refused failed")
  _run(REFUSED ENV --unset=FMINUS_WEB_ALLOW COMMAND "${FMINUS_WORK_DIR}/refused${FMINUS_EXE_SUFFIX}")
  _expect("REFUSED_STATUS;EQUAL;0" "the executable that calls the web library failed")
  _expect("REFUSED_OUT;STREQUAL;1\n" "the executable that calls the web library printed '${REFUSED_OUT}', not 1")
endif()

# ---- what the installed fminus says when it cannot find what it links with -------------------------------------------
_run(NOWHERE ENV "FMINUS_RUNTIME_DIR=${FMINUS_WORK_DIR}/nowhere" COMMAND "${_fminus}" "${_note}" -o never)
_expect("NOT;NOWHERE_STATUS;EQUAL;0" "linking with FMINUS_RUNTIME_DIR naming nothing succeeded")
_expect("NOWHERE_ERR;MATCHES;cannot find the FMinus runtime: no [^\n]*nowhere"
        "linking with FMINUS_RUNTIME_DIR naming nothing did not say so")
_run(NODRIVER ENV "FMINUS_LINK_DRIVER=${FMINUS_WORK_DIR}/no-such-driver" COMMAND "${_fminus}" "${_note}" -o never)
_expect("NOT;NODRIVER_STATUS;EQUAL;0" "linking with FMINUS_LINK_DRIVER naming nothing succeeded")
_expect("NODRIVER_ERR;MATCHES;no C\\+\\+ compiler to link with: tried [^\n]*no-such-driver \\(FMINUS_LINK_DRIVER\\)"
        "linking with FMINUS_LINK_DRIVER naming nothing did not say so")
_expect("NOT;EXISTS;${FMINUS_WORK_DIR}/never${FMINUS_EXE_SUFFIX}" "a failed link left an executable")

message(STATUS "install test: passed (${_moved})")
