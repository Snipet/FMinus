# tests/CMakeLists.txt
#
# Catch2: one executable, bin/fminus-tests, built from every .cpp under unit/ and every .cpp directly in smoke/. ctest
# runs each TEST_CASE as its own test; `bin/fminus-tests "[jit]"` runs a tag by hand, and `--benchmark-samples 100`
# tunes Catch2's BENCHMARKs.
#
# library/ tests the public API (include/fminus/FMinus.h) as a host uses it: its own executable,
# bin/fminus-library-tests, built in C++17 against FMinus::FMinus alone, without LLVM's headers or FMinus's internals.
#
# smoke/ checks the toolchain itself: LLJIT and the O3 pipeline on the host CPU (JitSmokeTest.cpp), and Bison + Flex
# through fminus_add_grammar (smoke/calc/, a reentrant scanner feeding a C++ variant parser: a pattern to copy).
#
# On Windows both Catch2 executables carry utf8.manifest: UTF-8 as their code page, so that ctest's filter, a test's
# whole name, reaches Catch2 as the TEST_CASE spells it, "§" and all (the console's code page would garble it, and the
# test would match nothing and fail).
#
# install/ tests the install (packaging/CMakeLists.txt): InstallTest.cmake, a CMake script, the test "install".
#
# lit: every *.test, *.fm and *.ll under lit/, through bin/fminus-lit, a lit wrapper that maps the source tree to this
# build tree. FMinus's tools, FileCheck, not, count and LLVM's tools are on PATH. ctest runs the suite as one test
# ("lit"); `bin/fminus-lit -v ../tests/lit/smoke` runs part of it by hand.

# ---- the toolchain smoke grammar ------------------------------------------------------------------------------------
add_library(fminus_smoke_calc STATIC smoke/calc/CalcDriver.cpp smoke/calc/CalcScanner.h)
fminus_target_defaults(fminus_smoke_calc)
fminus_add_grammar(fminus_smoke_calc PARSER smoke/calc/Calc.y LEXERS smoke/calc/Calc.l)
target_include_directories(fminus_smoke_calc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- Catch2 ---------------------------------------------------------------------------------------------------------
file(GLOB_RECURSE _fminus_unit_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
file(GLOB _fminus_smoke_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/smoke/*.cpp")
add_executable(fminus-tests ${_fminus_unit_tests} ${_fminus_smoke_tests})
fminus_target_defaults(fminus-tests)
target_link_libraries(fminus-tests PRIVATE FMinus::Internal fminus_smoke_calc Catch2::Catch2WithMain)
catch_discover_tests(fminus-tests DISCOVERY_MODE PRE_TEST)
# KeepIrTest.cpp runs the fminus driver beside this executable (bin/), so building the tests builds it too.
add_dependencies(fminus-tests fminus)

# ---- the public API, as a host uses it ------------------------------------------------------------------------------
file(GLOB_RECURSE _fminus_library_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/library/*.cpp")
add_executable(fminus-library-tests ${_fminus_library_tests})
fminus_target_defaults(fminus-library-tests CXX_STANDARD 17)
target_link_libraries(fminus-library-tests PRIVATE FMinus::FMinus Catch2::Catch2WithMain)
catch_discover_tests(fminus-library-tests DISCOVERY_MODE PRE_TEST)

if(WIN32)
  # Merged into each executable's manifest by the link step (CMake's MSVC rules, which clang-cl takes too).
  target_sources(fminus-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/utf8.manifest")
  target_sources(fminus-library-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/utf8.manifest")
endif()

# ---- the install ----------------------------------------------------------------------------------------------------
# install/InstallTest.cmake, the ctest test "install": installs the build (packaging/CMakeLists.txt) into install-test/
# beside this file's build directory, moves the prefix, and runs the installed fminus from there (--version,
# --print-runtime, --run, an executable made and run).
if(TARGET fminus)
  add_test(NAME install COMMAND "${CMAKE_COMMAND}"
    "-DFMINUS_BUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DFMINUS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-test"
    "-DFMINUS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DFMINUS_VERSION=${PROJECT_VERSION}"
    "-DFMINUS_CONFIG=$<CONFIG>"
    "-DFMINUS_BIN_DIR=${CMAKE_INSTALL_BINDIR}"
    "-DFMINUS_RUNTIME_DIR_REL=${FMINUS_INSTALL_RUNTIME_DIR}"
    "-DFMINUS_DOC_DIR=${FMINUS_INSTALL_DOC_DIR}"
    "-DFMINUS_RUNTIME_NAME=$<TARGET_FILE_NAME:fminus_runtime>"
    "-DFMINUS_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
    "-DFMINUS_WEB=${FMINUS_HAS_WEB}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/install/InstallTest.cmake")
endif()

# ---- lit ------------------------------------------------------------------------------------------------------------
if(FMINUS_LIT_PYTHON)
  set(FMINUS_LIT_SITE_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py")
  configure_file(lit/lit.site.cfg.py.in "${FMINUS_LIT_SITE_CONFIG}" @ONLY)
  configure_file(lit/fminus-lit.py.in "${PROJECT_BINARY_DIR}/bin/fminus-lit" @ONLY
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                  WORLD_READ WORLD_EXECUTE)
  # What lit runs must exist before it runs: our tools and, inline, LLVM's (built as part of `all`).
  add_custom_target(fminus-lit-deps ALL DEPENDS ${FMINUS_TOOL_TARGETS} ${FMINUS_LLVM_TOOL_TARGETS})
  add_test(NAME lit COMMAND "${FMINUS_LIT_PYTHON}" "${PROJECT_BINARY_DIR}/bin/fminus-lit" -sv
                            "${CMAKE_CURRENT_SOURCE_DIR}/lit")
  add_custom_target(check-lit
    COMMAND "${FMINUS_LIT_PYTHON}" "${PROJECT_BINARY_DIR}/bin/fminus-lit" -v "${CMAKE_CURRENT_SOURCE_DIR}/lit"
    USES_TERMINAL
    COMMENT "lit tests/lit")
  add_dependencies(check-lit fminus-lit-deps)
endif()

# `cmake --build <tree> --target check-fminus`: build what the tests need, then run every test.
add_custom_target(check-fminus
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${PROJECT_BINARY_DIR}" --output-on-failure
  USES_TERMINAL
  COMMENT "ctest")
add_dependencies(check-fminus fminus-tests fminus-library-tests)
if(FMINUS_EXAMPLE_TARGETS)
  add_dependencies(check-fminus ${FMINUS_EXAMPLE_TARGETS})
endif()
if(TARGET fminus-lit-deps)
  add_dependencies(check-fminus fminus-lit-deps)
endif()
