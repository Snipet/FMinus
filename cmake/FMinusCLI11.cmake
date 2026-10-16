# cmake/FMinusCLI11.cmake: CLI11 (FetchContent, header-only), the command-line parser of the executables under tools/.
# Included only when FMINUS_BUILD_TOOLS is ON. Gives CLI11::CLI11; tools/CMakeLists.txt links it into every tool.
include(FetchContent)

set(CLI11_PRECOMPILED OFF)
set(CLI11_BUILD_TESTS OFF)
set(CLI11_BUILD_EXAMPLES OFF)
set(CLI11_BUILD_DOCS OFF)
set(CLI11_INSTALL OFF)
FetchContent_Declare(CLI11
  URL "${FMINUS_CLI11_URL}"
  URL_HASH SHA256=${FMINUS_CLI11_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(CLI11)
