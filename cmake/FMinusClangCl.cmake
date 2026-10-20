# cmake/FMinusClangCl.cmake: Windows' compiler, found before project(). FMinus builds with clang-cl there, clang's
# driver for MSVC's command line (cmake/FMinusOptions.cmake says why), run from a Visual Studio developer prompt for
# x64, whose environment names MSVC's and the Windows SDK's headers and libraries. CMakeLists.txt includes this when no
# compiler was chosen (CC, CXX, -DCMAKE_<LANG>_COMPILER) and the generator is not Visual Studio's (which takes
# -T ClangCL).
#
# It refuses a shell that is not an x64 developer prompt, where the build would get as far as linking (LLVM's, half an
# hour in) before failing. Then it looks for clang-cl in that prompt's Visual Studio, whose "C++ Clang Compiler for
# Windows" matches its C++ library; then on PATH; then in LLVM's installer's directory. It sets CMAKE_C_COMPILER and
# CMAKE_CXX_COMPILER to the one it finds.

if(NOT DEFINED ENV{VCToolsInstallDir})
  message(FATAL_ERROR
    "FMinus: on Windows, configure from a Visual Studio developer prompt for x64: open \"x64 Native Tools Command "
    "Prompt for VS\" from the Start menu (or, in PowerShell, run Visual Studio's Common7\\Tools\\Launch-VsDevShell.ps1 "
    "-Arch amd64), go to this checkout, delete any build-<preset> directory a failed configure left, and run the "
    "command again. The prompt's environment names MSVC's and the Windows SDK's headers and libraries, which the "
    "compiler and the linker need. (docs/building.md, Windows)")
endif()
if(DEFINED ENV{VSCMD_ARG_TGT_ARCH} AND NOT "$ENV{VSCMD_ARG_TGT_ARCH}" MATCHES "^(x64|arm64)$")
  message(FATAL_ERROR
    "FMinus: this developer prompt targets $ENV{VSCMD_ARG_TGT_ARCH}, and FMinus builds for x64 (or arm64): open "
    "\"x64 Native Tools Command Prompt for VS\" instead (Launch-VsDevShell.ps1 -Arch amd64 in PowerShell), delete "
    "any build-<preset> directory a failed configure left, and run the command again.")
endif()

if("$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
  set(_fminus_llvm_host ARM64)
else()
  set(_fminus_llvm_host x64)
endif()
cmake_path(SET _fminus_vs_llvm NORMALIZE "$ENV{VCINSTALLDIR}")
cmake_path(APPEND _fminus_vs_llvm Tools Llvm ${_fminus_llvm_host} bin)
cmake_path(SET _fminus_llvm_installer NORMALIZE "$ENV{ProgramFiles}")
cmake_path(APPEND _fminus_llvm_installer LLVM bin)
find_program(FMINUS_CLANG_CL clang-cl HINTS "${_fminus_vs_llvm}" NO_DEFAULT_PATH)   # this prompt's Visual Studio
find_program(FMINUS_CLANG_CL clang-cl PATHS "${_fminus_llvm_installer}")           # PATH, then LLVM's installer
if(NOT FMINUS_CLANG_CL)
  message(FATAL_ERROR
    "FMinus: no clang-cl, in this prompt's Visual Studio (${_fminus_vs_llvm}), on PATH or in "
    "${_fminus_llvm_installer}. Add it with the Visual Studio Installer: Modify, then \"C++ Clang tools for Windows\" "
    "in the \"Desktop development with C++\" workload (\"C++ Clang Compiler for Windows\" under Individual "
    "components). Or install LLVM. Then open a new developer prompt, delete any build-<preset> directory a failed "
    "configure left, and run the command again.")
endif()
message(STATUS "FMinus: clang-cl: ${FMINUS_CLANG_CL}")
set(CMAKE_C_COMPILER "${FMINUS_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${FMINUS_CLANG_CL}")
