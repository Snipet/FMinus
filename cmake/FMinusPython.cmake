# cmake/FMinusPython.cmake: what the Python package needs (python/, docs/python.md): the Python it is built for, and
# pybind11 (FetchContent, header-only), which its extension module is written with. Included only when
# FMINUS_BUILD_PYTHON is ON.
#
# The Python is the system's, as the C++ compiler is: its interpreter and the headers an extension module compiles
# against (python3-dev on Debian and Ubuntu; python.org's and Homebrew's installs have them). -DPython_EXECUTABLE=<path>
# picks another than the first on PATH, a virtual environment's say; pip's build (pyproject.toml) passes its own.
#
# Gives pybind11_add_module() and Python_EXECUTABLE.
include(FetchContent)

find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

set(PYBIND11_FINDPYTHON ON)   # CMake's FindPython (above), not pybind11's own search
set(PYBIND11_INSTALL OFF)
set(PYBIND11_TEST OFF)
FetchContent_Declare(pybind11
  URL "${FMINUS_PYBIND11_URL}"
  URL_HASH SHA256=${FMINUS_PYBIND11_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(pybind11)
message(STATUS "FMinus: Python ${Python_VERSION} (${Python_EXECUTABLE}), pybind11 ${FMINUS_PYBIND11_VERSION}")
