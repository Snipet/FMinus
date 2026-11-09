# python/CMakeLists.txt: the Python package, `import fminus` (docs/python.md). Built when FMINUS_BUILD_PYTHON is ON.
#
# The package is the directory <build>/python/fminus/: the extension module _fminus (src/*.cpp, written with pybind11
# against the public API alone, in C++17, as a host is: FMinus::FMinus, with FMinus, its runtime and LLVM linked into
# it) beside a copy of fminus/*.py, the Python around it. Put <build>/python on PYTHONPATH to import it from the build
# tree; `pip install .` (pyproject.toml) installs the same two things, through the install rules below.
#
# tests/ holds its tests, unittest's (the standard library's: nothing to install): ctest runs them as one test,
# "python", and `PYTHONPATH=<build>/python python -m unittest discover -s python/tests -v` by hand.
# Adding a source (src/*.cpp), a module (fminus/*.py) or a test (tests/test_*.py) edits no CMake.

set(_fminus_python_package "${PROJECT_BINARY_DIR}/python/fminus")

file(GLOB _fminus_python_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
                                                   "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
# NO_EXTRAS: without pybind11's link-time optimisation and stripping, which depend on the linker at hand.
pybind11_add_module(fminus_python MODULE NO_EXTRAS ${_fminus_python_sources})
fminus_target_defaults(fminus_python CXX_STANDARD 17)
target_link_libraries(fminus_python PRIVATE FMinus::FMinus)
# $<1:...>: the same directory in every configuration of a multi-config generator (Xcode, Visual Studio).
set_target_properties(fminus_python PROPERTIES
  OUTPUT_NAME _fminus
  LIBRARY_OUTPUT_DIRECTORY "$<1:${_fminus_python_package}>")
# The module holds LLVM, linked statically. Its symbols stay its own, out of the module's dynamic symbol table, so that
# they meet no other LLVM in the process (another extension module's). On macOS and Windows a module's symbols are its
# own already.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(fminus_python PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# The package's Python files, copied beside the module when they change.
file(GLOB _fminus_python_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fminus/*.py")
set(_fminus_python_copies "")
foreach(_file IN LISTS _fminus_python_files)
  cmake_path(GET _file FILENAME _name)
  add_custom_command(OUTPUT "${_fminus_python_package}/${_name}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_file}" "${_fminus_python_package}/${_name}"
    DEPENDS "${_file}"
    COMMENT "python/fminus/${_name}"
    VERBATIM)
  list(APPEND _fminus_python_copies "${_fminus_python_package}/${_name}")
endforeach()
add_custom_target(fminus-python ALL DEPENDS ${_fminus_python_copies})
add_dependencies(fminus-python fminus_python)

# What pip installs (pyproject.toml: the component "python", into the wheel's root).
install(TARGETS fminus_python LIBRARY DESTINATION fminus COMPONENT python)
install(FILES ${_fminus_python_files} DESTINATION fminus COMPONENT python)

if(FMINUS_BUILD_TESTS)
  add_test(NAME python
    COMMAND "${Python_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -v)
  set_tests_properties(python PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_BINARY_DIR}/python;FMINUS_EXAMPLES_DIR=${PROJECT_SOURCE_DIR}/examples")
  if(TARGET check-fminus)
    add_dependencies(check-fminus fminus-python)
  endif()
endif()
