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
# Catch2 in C++17. It asks for C++14 alone, so it is built in the compiler's default, and clang-cl's is C++14, where
# Catch2 compiles none of its C++17 support (StringMaker<std::string_view>, ...) into the library that the tests' C++17
# and C++23 headers declare: they would not link. C++17 is GCC's and clang's default elsewhere, and the oldest standard
# a test is built in (fminus-library-tests); what C++20 adds to Catch2 is in its headers alone.
set_target_properties(Catch2 Catch2WithMain PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
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
