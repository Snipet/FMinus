# examples/plugin/CMakeLists.txt: FMinus Filter, a JUCE plugin (AU, VST3 and Standalone, on macOS) whose DSP is an
# FMinus program, filter.fm, which its processor JIT-compiles (README.md). It is a CMake project of its own, not part of
# FMinus's build (examples/CMakeLists.txt skips a directory with a CMakeLists.txt), run from the checkout's root:
#
#   cmake -S examples/plugin -B build-plugin [-DFETCHCONTENT_SOURCE_DIR_JUCE=<a JUCE 8.0.4 checkout>]
#   cmake --build build-plugin -j6
#   ctest --test-dir build-plugin      # fminus-plugin-check, the headless check (Check.cpp)
#
# Dependencies:
#   JUCE 8.0.4  FetchContent, pinned by commit in ../../cmake/FMinusPins.cmake (FMINUS_JUCE_*); its version, its commit
#               and its tracked files are checked below, whatever its source. FETCHCONTENT_SOURCE_DIR_JUCE builds with
#               a local checkout.
#   FMinus      this checkout, as a subproject: its library and runtime only (a subproject builds no tests, tools or
#               examples), linked as FMinus::FMinus, whose one header, fminus/FMinus.h, has no LLVM in it.
#   LLVM        FMinus's "system" mode, on the install FMinus's own build made in its deps cache (FMINUS_DEPS_DIR,
#               default this checkout's .deps/): a checkout that has built FMinus once builds the plugin without
#               building LLVM again. Only a Release LLVM is taken from there; -DLLVM_DIR=<install>/lib/cmake/llvm names
#               any other by hand.
#   Bison, Flex FMinus's "fetch" mode: the ones installed in the same deps cache (built there in a minute if missing).
cmake_minimum_required(VERSION 3.30)   # FMinus's own minimum (CMP0168: FetchContent without a sub-build)

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")

project(FMinusFilter
  VERSION 0.1.0
  DESCRIPTION "A JUCE plugin whose DSP is an FMinus program"
  LANGUAGES C CXX)

if(NOT APPLE)
  message(FATAL_ERROR "FMinus Filter: this example is written for macOS (AU, VST3 and Standalone bundles, each with an "
                      "exported-symbols list); FMinus itself builds on Linux and Windows too")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(FMINUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${FMINUS_ROOT}/cmake/FMinusPins.cmake")
include(FetchContent)
find_package(Git REQUIRED)

# ---- JUCE ------------------------------------------------------------------------------------------------------------
# Pinned by commit (FMinusPins.cmake says why): the whole repository is cloned, as git checks out a commit only from
# its history. FETCHCONTENT_SOURCE_DIR_JUCE skips the clone, and GIT_TAG with it, so the version, the commit and the
# tracked files are checked here, after population, whatever the source.
FetchContent_Declare(JUCE GIT_REPOSITORY ${FMINUS_JUCE_REPOSITORY} GIT_TAG ${FMINUS_JUCE_COMMIT})
FetchContent_MakeAvailable(JUCE)

file(STRINGS "${juce_SOURCE_DIR}/modules/juce_core/system/juce_StandardHeader.h" _juce_version_lines
     REGEX "^#define JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER) +[0-9]+")
set(_juce_version "")
foreach(_part MAJOR_VERSION MINOR_VERSION BUILDNUMBER)
  string(REGEX MATCH "#define JUCE_${_part} +([0-9]+)" _ "${_juce_version_lines}")
  list(APPEND _juce_version "${CMAKE_MATCH_1}")
endforeach()
list(JOIN _juce_version "." _juce_version)
if(NOT _juce_version STREQUAL FMINUS_JUCE_VERSION)
  message(FATAL_ERROR "FMinus Filter: JUCE at ${juce_SOURCE_DIR} is ${_juce_version}; the pin is "
                      "${FMINUS_JUCE_VERSION} (cmake/FMinusPins.cmake)")
endif()
# The commit and the files: read-only git (--no-optional-locks: the index is refreshed in memory, never written). A
# JUCE that is not the root of a git checkout (an extracted archive) cannot be checked by commit, and only its version
# is.
set(_git ${CMAKE_COMMAND} -E env --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
         ${GIT_EXECUTABLE} --no-optional-locks -C "${juce_SOURCE_DIR}")
execute_process(COMMAND ${_git} rev-parse --show-toplevel RESULT_VARIABLE _rc OUTPUT_VARIABLE _juce_top
                ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_rc EQUAL 0)
  file(REAL_PATH "${_juce_top}" _juce_top)
  file(REAL_PATH "${juce_SOURCE_DIR}" _juce_dir)
endif()
if(_rc EQUAL 0 AND _juce_top STREQUAL _juce_dir)
  execute_process(COMMAND ${_git} rev-parse -q --verify "HEAD^{commit}" OUTPUT_VARIABLE _juce_head ERROR_QUIET
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _juce_head STREQUAL FMINUS_JUCE_COMMIT)
    message(FATAL_ERROR "FMinus Filter: JUCE at ${juce_SOURCE_DIR} is at commit '${_juce_head}'; the pin is "
                        "${FMINUS_JUCE_COMMIT} (JUCE ${FMINUS_JUCE_VERSION}, cmake/FMinusPins.cmake)")
  endif()
  # HEAD names the commit, not the files: a tracked file changed since is not the pin either. Untracked files (a build
  # directory inside the checkout, say) are left alone: JUCE's CMake compiles only its modules' own files.
  execute_process(COMMAND ${_git} status --porcelain --untracked-files=no RESULT_VARIABLE _rc
                  OUTPUT_VARIABLE _juce_changed ERROR_VARIABLE _juce_changed OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0 OR NOT _juce_changed STREQUAL "")
    string(REPLACE "\n" ";" _juce_changed "${_juce_changed}")
    list(LENGTH _juce_changed _count)
    list(SUBLIST _juce_changed 0 5 _juce_changed)
    list(JOIN _juce_changed "\n  " _juce_changed)
    message(FATAL_ERROR "FMinus Filter: JUCE at ${juce_SOURCE_DIR} is at the pinned commit, but git status shows "
                        "${_count} tracked file(s) changed since, so it is not the pin:\n  ${_juce_changed}\nUse a "
                        "clean checkout of ${FMINUS_JUCE_COMMIT}, or leave FETCHCONTENT_SOURCE_DIR_JUCE unset to clone "
                        "one.")
  endif()
else()
  message(WARNING "FMinus Filter: JUCE at ${juce_SOURCE_DIR} is not a git checkout: its version is "
                  "${_juce_version}, but its commit cannot be checked against ${FMINUS_JUCE_COMMIT}")
endif()

# AppleClang 21 warns (-Wnontrivial-memcall) inside the HarfBuzz that JUCE 8.0.4 bundles. Third-party code, compiled
# into this project's targets: silenced in JUCE's own module sources only, where the compiler has the warning.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wnontrivial-memcall FMINUS_FILTER_HAS_WNONTRIVIAL_MEMCALL)
if(FMINUS_FILTER_HAS_WNONTRIVIAL_MEMCALL)
  file(GLOB _juce_module_sources "${juce_SOURCE_DIR}/modules/juce_*/juce_*.cpp"
                                 "${juce_SOURCE_DIR}/modules/juce_*/juce_*.mm")
  set_source_files_properties(${_juce_module_sources} DIRECTORY "${PROJECT_SOURCE_DIR}"
                              PROPERTIES COMPILE_OPTIONS -Wno-nontrivial-memcall)
endif()

# ---- FMinus, on an LLVM its own build installed ----------------------------------------------------------------------
if(DEFINED ENV{FMINUS_DEPS_DIR})
  set(_deps_default "$ENV{FMINUS_DEPS_DIR}")
else()
  set(_deps_default "${FMINUS_ROOT}/.deps")
endif()
set(FMINUS_DEPS_DIR "${_deps_default}" CACHE PATH
    "FMinus's deps cache, where FMinus's own build installed LLVM, Bison and Flex")

# The LLVM install, a Release build of LLVM (llvm-<version>-Release[-assert]-<hash>; FMINUS_LLVM_BUILD_TYPE can make
# others, a Debug one to step into LLVM say, which are never taken from here: -DLLVM_DIR names one). A Release build of
# the plugin takes the flavor without assertions when there is one (FMinus's release preset builds it), any other build
# the one with them (its dev preset's), and either takes the other when that is all there is. FMinus writes
# fminus-flavor.cmake into an install once it is whole.
if(NOT LLVM_DIR)
  file(GLOB _plain LIST_DIRECTORIES true "${FMINUS_DEPS_DIR}/install/llvm-${FMINUS_LLVM_VERSION}-Release-[0-9a-f]*")
  file(GLOB _asserting LIST_DIRECTORIES true "${FMINUS_DEPS_DIR}/install/llvm-${FMINUS_LLVM_VERSION}-Release-assert-*")
  list(FILTER _plain EXCLUDE REGEX "(-assert-|\\.lock$)")   # [0-9a-f] matches assert's a
  list(FILTER _asserting EXCLUDE REGEX "\\.lock$")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_flavors ${_plain} ${_asserting})
  else()
    set(_flavors ${_asserting} ${_plain})
  endif()
  foreach(_flavor IN LISTS _flavors)
    if(EXISTS "${_flavor}/fminus-flavor.cmake" AND EXISTS "${_flavor}/lib/cmake/llvm/LLVMConfig.cmake")
      set(LLVM_DIR "${_flavor}/lib/cmake/llvm")
      break()
    endif()
  endforeach()
  if(NOT LLVM_DIR)
    message(FATAL_ERROR "FMinus Filter: no Release build of LLVM ${FMINUS_LLVM_VERSION} installed in "
                        "${FMINUS_DEPS_DIR}/install. Build FMinus once at ${FMINUS_ROOT} (cmake --workflow --preset "
                        "dev: it builds LLVM into its deps cache, in 10-30 minutes), set FMINUS_DEPS_DIR to a deps "
                        "cache that has one, or pass -DLLVM_DIR=<an LLVM ${FMINUS_LLVM_VERSION} "
                        "install>/lib/cmake/llvm (the way to use an LLVM of another build type too).")
  endif()
endif()
if(NOT EXISTS "${LLVM_DIR}/LLVMConfigVersion.cmake")
  message(FATAL_ERROR "FMinus Filter: LLVM_DIR (${LLVM_DIR}) holds no LLVMConfigVersion.cmake")
endif()
file(STRINGS "${LLVM_DIR}/LLVMConfigVersion.cmake" _llvm_version REGEX "^set\\(PACKAGE_VERSION ")
string(REGEX REPLACE "^set\\(PACKAGE_VERSION \"?([^\")]*)\"?\\)$" "\\1" _llvm_version "${_llvm_version}")
if(NOT _llvm_version STREQUAL FMINUS_LLVM_VERSION)
  message(FATAL_ERROR "FMinus Filter: the LLVM at ${LLVM_DIR} is ${_llvm_version}; FMinus pins "
                      "${FMINUS_LLVM_VERSION} (cmake/FMinusPins.cmake)")
endif()

# FMinus's options, as normal variables before its CMakeLists.txt reads them (CMP0077 for its options, CMP0126 for its
# cache variables).
set(FMINUS_LLVM_MODE system)
set(FMINUS_PARSER_TOOLS_MODE fetch)
set(FMINUS_WERROR OFF)                 # warnings in FMinus's sources are not errors in this project
add_subdirectory("${FMINUS_ROOT}" fminus EXCLUDE_FROM_ALL)
message(STATUS "FMinus Filter: JUCE ${_juce_version} at ${juce_SOURCE_DIR}; FMinus at ${FMINUS_ROOT}, on LLVM "
               "${_llvm_version} at ${LLVM_DIR}")

# ---- the plugin ------------------------------------------------------------------------------------------------------
# The warnings of this example's own sources: JUCE's recommended list (juce::juce_recommended_warning_flags) with
# -Wextra, but per file, so that JUCE's module sources, which compile inside the same targets, keep their own flags.
set(FMINUS_FILTER_WARNINGS
    -Wall -Wextra -Wshadow-all -Wshorten-64-to-32 -Wstrict-aliasing -Wuninitialized -Wunused-parameter -Wconversion
    -Wsign-compare -Wint-conversion -Wconditional-uninitialized -Wconstant-conversion -Wsign-conversion
    -Wbool-conversion -Wextra-semi -Wunreachable-code -Wcast-align -Wshift-sign-overflow -Wmissing-prototypes
    -Wnullable-to-nonnull-conversion -Wno-ignored-qualifiers -Wswitch-enum -Wpedantic -Wdeprecated -Wfloat-equal
    -Wmissing-field-initializers -Wzero-as-null-pointer-constant -Wunused-private-field -Woverloaded-virtual -Wreorder
    -Winconsistent-missing-destructor-override)
set(FMINUS_FILTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Processor.cpp")
set(FMINUS_FILTER_CHECK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Check.cpp")
set_source_files_properties(${FMINUS_FILTER_SOURCES} ${FMINUS_FILTER_CHECK_SOURCES}
                            PROPERTIES COMPILE_OPTIONS "${FMINUS_FILTER_WARNINGS}")

# filter.fm, embedded: FilterSource.h declares FilterSource::filter_fm and FilterSource::filter_fmSize.
juce_add_binary_data(FMinusFilterSource HEADER_NAME FilterSource.h NAMESPACE FilterSource
                     SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/filter.fm")

# The codes: the manufacturer's, Fmns, is FMinus's examples', and the plugin's, Fflt, is this one's (AU hosts tell
# plugins apart by the pair, so another example takes another plugin code).
juce_add_plugin(FMinusFilter
  PRODUCT_NAME "FMinus Filter"
  COMPANY_NAME "FMinus"
  BUNDLE_ID io.github.snipet.fminus.filter
  PLUGIN_MANUFACTURER_CODE Fmns
  PLUGIN_CODE Fflt
  FORMATS AU VST3 Standalone
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  VST3_CATEGORIES Fx Filter
  AU_MAIN_TYPE kAudioUnitType_Effect
  AU_EXPORT_PREFIX FMinusFilterAU
  MICROPHONE_PERMISSION_ENABLED TRUE   # the Standalone filters its audio input
  MICROPHONE_PERMISSION_TEXT "FMinus Filter filters the audio input."
  COPY_PLUGIN_AFTER_BUILD FALSE)       # nothing is installed into ~/Library/Audio/Plug-Ins
target_sources(FMinusFilter PRIVATE ${FMINUS_FILTER_SOURCES})
# PUBLIC, so the format wrappers compile JUCE with the same settings as the shared code.
target_compile_definitions(FMinusFilter PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(FMinusFilter
  PRIVATE FMinusFilterSource FMinus::FMinus juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags)
# Every bundle (the format targets link the shared code, FMinusFilter, and take its interface): the linker drops the
# code nothing reaches, as Xcode's builds do, which is a fifth of a bundle here, most of it LLVM's.
target_link_options(FMinusFilter INTERFACE "LINKER:-dead_strip")

# Each plugin bundle exports its entry points and nothing else: the AU its two factories, the VST3 the functions a
# host looks up. LLVM is linked into the bundle statically, with FMinus, and its symbols, many of them weak, would
# otherwise be exported too: macOS merges weak definitions across the images loaded into one process, so two plugins
# that each bring an LLVM (or a host that has its own) would share one copy's functions and state. The Standalone is an
# application, its own process, and is left as it is.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/exports-au.txt" CONTENT "_JuceAUFactory\n_FMinusFilterAUFactory\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/exports-vst3.txt"
     CONTENT "_GetPluginFactory\n_bundleEntry\n_bundleExit\n")
foreach(_format AU VST3)
  string(TOLOWER ${_format} _list)
  set(_list "${CMAKE_CURRENT_BINARY_DIR}/exports-${_list}.txt")
  target_link_options(FMinusFilter_${_format} PRIVATE "LINKER:-exported_symbols_list,${_list}")
  set_property(TARGET FMinusFilter_${_format} APPEND PROPERTY LINK_DEPENDS "${_list}")
endforeach()

# Each bundle's last step signs it, ad hoc, over all its files. JUCE signs a bundle only in its copy step, which
# COPY_PLUGIN_AFTER_BUILD FALSE leaves out, and the VST3 before juce_vst3_helper adds Contents/Resources/moduleinfo.json
# to it: without this, the AU and the app keep the linker's signature, which seals no resources, and the VST3's seal
# is broken, so codesign --verify rejects all three. A POST_BUILD command added here runs after JUCE's own.
find_program(FMINUS_FILTER_CODESIGN codesign REQUIRED)
foreach(_format AU VST3 Standalone)
  add_custom_command(TARGET FMinusFilter_${_format} POST_BUILD
    COMMAND "${FMINUS_FILTER_CODESIGN}" --force --sign - "$<TARGET_BUNDLE_DIR:FMinusFilter_${_format}>"
    VERBATIM)
endforeach()

# ---- the check -------------------------------------------------------------------------------------------------------
# fminus-plugin-check: the processor, headless, through processBlock (Check.cpp). It compiles Processor.cpp itself,
# since JUCE's modules compile into each target that links them.
juce_add_console_app(fminus-plugin-check PRODUCT_NAME fminus-plugin-check)
target_sources(fminus-plugin-check PRIVATE ${FMINUS_FILTER_CHECK_SOURCES} ${FMINUS_FILTER_SOURCES})
target_compile_definitions(fminus-plugin-check PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(fminus-plugin-check
  PRIVATE FMinusFilterSource FMinus::FMinus juce::juce_audio_processors
  PUBLIC juce::juce_recommended_config_flags)
target_link_options(fminus-plugin-check PRIVATE "LINKER:-dead_strip")   # as the bundles are linked

enable_testing()
add_test(NAME plugin.check COMMAND fminus-plugin-check)
