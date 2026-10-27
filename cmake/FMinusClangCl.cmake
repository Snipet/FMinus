# cmake/FMinusClangCl.cmake: Windows' compiler, found before project(). FMinus builds with clang-cl there, clang's
# driver for MSVC's command line (cmake/FMinusOptions.cmake says why). CMakeLists.txt includes this when no compiler
# was chosen (CC, CXX, -DCMAKE_<LANG>_COMPILER) and the generator is not Visual Studio's (which takes -T ClangCL).
#
# It looks for clang-cl in Visual Studio first, whose "C++ Clang Compiler for Windows" matches its C++ library: in a
# developer prompt, that prompt's Visual Studio; in any other prompt, the newest one with the component (vswhere).
# There, though, clang-cl takes the C++ library of the newest Visual Studio installed, with the component or not, and a
# newer library may refuse an older clang (STL1000; docs/building.md, "Two Visual Studios").
# Then on PATH, then in LLVM's installer's directory. It sets CMAKE_C_COMPILER and CMAKE_CXX_COMPILER to the one it
# finds. clang-cl finds MSVC's and the Windows SDK's headers and libraries itself when no developer prompt names them.
# A developer prompt for x86 is refused: its libraries are 32-bit, and FMinus builds for x64 (or arm64).

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
set(_fminus_vs "")
if(DEFINED ENV{VCINSTALLDIR})                                    # a developer prompt: its Visual Studio
  cmake_path(SET _fminus_vs NORMALIZE "$ENV{VCINSTALLDIR}")
  cmake_path(GET _fminus_vs PARENT_PATH _fminus_vs)              # ...\VC\ -> ...\VC, the installation's VC directory
else()                                                           # any other prompt: the newest Visual Studio with it
  set(_fminus_pf86 "ProgramFiles(x86)")
  set(_fminus_vswhere "$ENV{${_fminus_pf86}}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${_fminus_vswhere}")
    execute_process(COMMAND "${_fminus_vswhere}" -latest -products * -property installationPath
                            -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang
                    OUTPUT_VARIABLE _fminus_vs OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_fminus_vs)
      cmake_path(SET _fminus_vs NORMALIZE "${_fminus_vs}")
      cmake_path(APPEND _fminus_vs VC)
    endif()
  endif()
endif()
set(_fminus_vs_llvm "")
if(_fminus_vs)
  cmake_path(APPEND _fminus_vs Tools Llvm ${_fminus_llvm_host} bin OUTPUT_VARIABLE _fminus_vs_llvm)
  find_program(FMINUS_CLANG_CL clang-cl HINTS "${_fminus_vs_llvm}" NO_DEFAULT_PATH)
endif()
cmake_path(SET _fminus_llvm_installer NORMALIZE "$ENV{ProgramFiles}")
cmake_path(APPEND _fminus_llvm_installer LLVM bin)
find_program(FMINUS_CLANG_CL clang-cl PATHS "${_fminus_llvm_installer}")   # PATH, then LLVM's installer
if(NOT FMINUS_CLANG_CL)
  set(_fminus_where "")
  if(_fminus_vs_llvm)
    set(_fminus_where "in ${_fminus_vs_llvm}, ")
  endif()
  message(FATAL_ERROR
    "FMinus: no clang-cl ${_fminus_where}on PATH or in ${_fminus_llvm_installer}. Add it with "
    "the Visual Studio Installer: Modify, then \"C++ Clang tools for Windows\" in the \"Desktop development with C++\" "
    "workload (\"C++ Clang Compiler for Windows\" under Individual components). Or install LLVM. Then delete any "
    "build-<preset> directory a failed configure left, and run the command again.")
endif()
message(STATUS "FMinus: clang-cl: ${FMINUS_CLANG_CL}")
set(CMAKE_C_COMPILER "${FMINUS_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${FMINUS_CLANG_CL}")
