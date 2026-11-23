# The desktop experiment's build (docs/research/desktop/README.md, section 12): research code, not part of FMinus's
# build. One project that adds FunkGui (its JUCE-free core) and FMinus as subprojects, and builds host.cpp twice:
# counter-jit, which holds the compiler, and counter-aot, which links the object file `fminus -c` wrote.
#
#   cmake -S docs/research/desktop/experiments -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DFUNKGUI_DIR=<a FunkGui checkout> -DFMINUS_EXE=<a built fminus>      (and, to reuse a checkout's LLVM:)
#         -DFMINUS_LLVM_MODE=system -DLLVM_DIR=<checkout>/.deps/install/llvm-<version>-<flavor>/lib/cmake/llvm \
#         -DFMINUS_DEPS_DIR=<checkout>/.deps
cmake_minimum_required(VERSION 3.30)
project(FMinusDesktopExperiment LANGUAGES C CXX)

set(FUNKGUI_DIR "" CACHE PATH "A FunkGui checkout (v0.27.0 here)")
set(FMINUS_EXE "" CACHE FILEPATH "A built fminus, to compile counter.fm ahead of time")
if(NOT FUNKGUI_DIR OR NOT FMINUS_EXE)
  message(FATAL_ERROR "Set FUNKGUI_DIR (a FunkGui checkout) and FMINUS_EXE (a built fminus)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FUNKGUI_WITH_JUCE OFF)             # FunkGui::core alone: no JUCE, no bgfx, nothing fetched
add_subdirectory("${FUNKGUI_DIR}" funkgui)

set(FMINUS_WERROR OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../.." fminus)

find_package(ZLIB REQUIRED)            # the PNGs: FunkGui's own encoder is JUCE's

function(experiment_host target)
  target_link_libraries(${target} PRIVATE FunkGui::core ZLIB::ZLIB)
  funkgui_configure_product(${target} PRODUCT Counter OBJC_PREFIX Cntr ENV_PREFIX COUNTER_ PREFS_FOLDER Counter)
  target_compile_options(${target} PRIVATE -ffp-contract=off -ffunction-sections -fdata-sections)
  if(APPLE)                            # not run: the experiment ran on Linux only
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
endfunction()

# The JIT host: the compiler inside, and its own ui_* functions exported, for the JIT to find by name.
add_executable(counter-jit host.cpp)
target_compile_definitions(counter-jit PRIVATE APP_JIT=1)
target_link_libraries(counter-jit PRIVATE FMinus::FMinus)
set_target_properties(counter-jit PROPERTIES ENABLE_EXPORTS ON)
experiment_host(counter-jit)

# The ahead-of-time host: counter.fm to an object file, linked like any other.
set(_object "${CMAKE_CURRENT_BINARY_DIR}/counter.o")
add_custom_command(OUTPUT "${_object}"
  COMMAND "${FMINUS_EXE}" -c "${CMAKE_CURRENT_SOURCE_DIR}/counter.fm" -o "${_object}"
  DEPENDS counter.fm ui.fm
  COMMENT "fminus -c counter.fm"
  VERBATIM)
add_executable(counter-aot host.cpp "${_object}")
target_compile_definitions(counter-aot PRIVATE APP_JIT=0)
target_link_libraries(counter-aot PRIVATE FMinus::Runtime)
experiment_host(counter-aot)
