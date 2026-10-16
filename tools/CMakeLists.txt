# tools/CMakeLists.txt: every directory under tools/ that holds .cpp files is one executable named after it
# (tools/fminus-hostinfo/ -> bin/fminus-hostinfo), linked with FMinus::Internal and CLI11::CLI11 (its command line;
# fminus-hostinfo shows the pattern). Adding a tool edits no CMake.
# FMINUS_TOOL_TARGETS lists them, so lit can depend on them.

file(GLOB_RECURSE _fminus_tool_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(_fminus_tools "")
foreach(_src IN LISTS _fminus_tool_sources)
  string(REGEX MATCH "^[^/]+" _tool "${_src}")
  if(_tool STREQUAL _src)
    message(FATAL_ERROR "FMinus: tools/${_src}: a tool's sources go in tools/<name>/")
  endif()
  list(APPEND _fminus_tools ${_tool})
  list(APPEND _fminus_tool_${_tool} "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
endforeach()
list(REMOVE_DUPLICATES _fminus_tools)

foreach(_tool IN LISTS _fminus_tools)
  add_executable(${_tool} ${_fminus_tool_${_tool}})
  fminus_target_defaults(${_tool})
  target_link_libraries(${_tool} PRIVATE FMinus::Internal CLI11::CLI11)
  list(APPEND FMINUS_TOOL_TARGETS ${_tool})
endforeach()
set(FMINUS_TOOL_TARGETS "${FMINUS_TOOL_TARGETS}" PARENT_SCOPE)
