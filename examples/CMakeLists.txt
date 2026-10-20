# examples/CMakeLists.txt: every directory under examples/ is one example. Its .cpp files build the executable
# example-<directory> (bin/example-biquad), linked with FMinus::Internal and CLI11, which finds its FMinus sources
# through FMINUS_EXAMPLE_DIR (the example's source directory). ctest runs each with --check. Adding an example edits no
# CMake. FMINUS_EXAMPLE_TARGETS lists them.

file(GLOB _fminus_example_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
set(FMINUS_EXAMPLE_TARGETS "")
foreach(_dir IN LISTS _fminus_example_dirs)
  if(NOT IS_DIRECTORY "${_dir}")
    continue()
  endif()
  file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${_dir}/*.cpp")
  if(NOT _sources)
    continue()
  endif()
  cmake_path(GET _dir FILENAME _name)
  set(_target "example-${_name}")
  add_executable(${_target} ${_sources})
  fminus_target_defaults(${_target})
  target_link_libraries(${_target} PRIVATE FMinus::Internal CLI11::CLI11)
  target_compile_definitions(${_target} PRIVATE "FMINUS_EXAMPLE_DIR=\"${_dir}\"")
  # An example's C++ reference must not fuse multiplies and adds where FMinus's IR does not. (clang-cl ignores clang's
  # own spelling, with a warning, and takes it through /clang:.)
  if(FMINUS_MSVC_FRONTEND)
    target_compile_options(${_target} PRIVATE /clang:-ffp-contract=off)
  else()
    target_compile_options(${_target} PRIVATE -ffp-contract=off)
  endif()
  if(FMINUS_BUILD_TESTS)
    add_test(NAME "example.${_name}" COMMAND ${_target} --check)
  endif()
  list(APPEND FMINUS_EXAMPLE_TARGETS ${_target})
endforeach()
set(FMINUS_EXAMPLE_TARGETS "${FMINUS_EXAMPLE_TARGETS}" PARENT_SCOPE)
