# cmake/FMinusTesting.cmake: Catch2 (FetchContent, built in this tree) and lit (from LLVM, run with Python 3). Included
# only when FMINUS_BUILD_TESTS is ON.
#
# Gives Catch2::Catch2WithMain, catch_discover_tests(), and FMINUS_LIT_PYTHON (the Python that runs lit, or empty when
# lit cannot run: the lit tests are then skipped with a message).
include(FetchContent)

set(CATCH_INSTALL_DOCS OFF)
set(CATCH_INSTALL_EXTRAS OFF)
FetchContent_Declare(Catch2
  URL "${FMINUS_CATCH2_URL}"
  URL_HASH SHA256=${FMINUS_CATCH2_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(Catch2)
# Catch2's library as C++17 at least, which its own CMake leaves at C++14: it has StringMaker<std::string_view> and the
# other conversions of C++17's types only then, and the tests (C++23) see them as there, so a comparison of two
# string_views would not link. Clang's default is C++17 already; clang-cl's, on Windows, is C++14.
target_compile_features(Catch2 PRIVATE cxx_std_17)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

set(FMINUS_LIT_PYTHON "")
find_package(Python3 3.8 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(STATUS "FMinus: no Python 3: the lit tests are skipped")
elseif(FMINUS_LIT_DIR)
  set(FMINUS_LIT_PYTHON "${Python3_EXECUTABLE}")
else()
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import lit" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
  if(_rc EQUAL 0)
    set(FMINUS_LIT_PYTHON "${Python3_EXECUTABLE}")
  else()
    message(STATUS "FMinus: this LLVM has no lit and ${Python3_EXECUTABLE} cannot import one (pip install lit): "
                   "the lit tests are skipped")
  endif()
endif()
