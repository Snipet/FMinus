# tests/lit/lit.cfg.py: FMinus's lit suite. Run it through the build tree's bin/fminus-lit (or ctest), which loads the
# generated lit.site.cfg.py first; that file holds the paths read here.
#
# A test is a file with RUN lines (lit's internal shell: pipes, `not`, `env`; FileCheck checks the output). FMinus's
# tools, then LLVM's (FileCheck, not, count, opt, llc, llvm-mca, ...), come first on PATH. Substitutions: %host_triple.
# Features for REQUIRES/UNSUPPORTED: asserts, asan, ubsan, tsan, the host's architecture (arm64 or x86_64; Windows
# spells them ARM64 and AMD64), its system (system-darwin, system-linux, system-windows), avx (an x86-64 host whose
# JIT targets AVX, as fminus-hostinfo --features lists it), json and xml (a build with the JSON parser, FMINUS_JSON, or
# the XML parser, FMINUS_XML, as the build tree's fminus/Config.h says).
import os
import platform
import shutil
import subprocess

import lit.formats
import lit.util

if not hasattr(config, "fminus_obj_root"):
    lit_config.fatal("run FMinus's lit tests through <build tree>/bin/fminus-lit (or ctest), not lit directly")

config.name = "FMinus"
config.test_format = lit.formats.ShTest(execute_external=False)
# .fm: FMinus sources (rename once the language's extension is settled); .ll: LLVM IR; .test: anything else.
config.suffixes = [".fm", ".ll", ".test"]
config.excludes = ["Inputs"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.fminus_obj_root, "tests", "lit")

config.environment["PATH"] = os.pathsep.join(
    [config.fminus_tools_dir, config.llvm_tools_dir, config.environment.get("PATH", "")])
for name in ("HOME", "TMPDIR", "ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"):
    if name in os.environ:
        config.environment[name] = os.environ[name]


# Windows has no echo or sed programs for RUN lines to pipe through (lit runs echo itself only outside a pipeline). As
# LLVM's own suites do, take Git for Windows' (its usr\bin) unless PATH has them: through its registry key, or beside
# the git on PATH. They go last on PATH, after FMinus's tools and LLVM's.
def git_unix_tools(tools):
    roots = []
    try:
        import winreg

        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for view in (0, winreg.KEY_WOW64_64KEY):
                try:
                    with winreg.OpenKey(hive, r"SOFTWARE\GitForWindows", 0, winreg.KEY_READ | view) as key:
                        roots.append(winreg.QueryValueEx(key, "InstallPath")[0])
                except OSError:
                    pass
    except ImportError:
        pass
    git = shutil.which("git")
    if git:
        roots.append(os.path.dirname(os.path.dirname(git)))   # <root>\cmd\git.exe or <root>\bin\git.exe
    for root in roots:
        candidate = os.path.join(root, "usr", "bin")
        if root and lit.util.checkToolsPath(candidate, tools):
            return candidate
    return None


if platform.system() == "Windows":
    unix_tools = ["echo.exe", "sed.exe"]
    if lit.util.whichTools(unix_tools, config.environment["PATH"]) is None:
        tools_dir = git_unix_tools(unix_tools)
        if tools_dir is None:
            lit_config.warning("no echo.exe and sed.exe on PATH or in Git for Windows: the tests whose RUN lines pipe "
                               "through them fail. Install Git for Windows, or put a directory with them on PATH.")
        else:
            config.environment["PATH"] = os.pathsep.join([config.environment["PATH"], tools_dir])

config.substitutions.append(("%host_triple", config.host_triple))

if config.llvm_assertions:
    config.available_features.add("asserts")
for sanitizer, feature in (("address", "asan"), ("undefined", "ubsan"), ("thread", "tsan")):
    if sanitizer in config.sanitizers:
        config.available_features.add(feature)
config.available_features.add(platform.machine())
config.available_features.add("system-" + platform.system().lower())   # system-darwin, system-linux, system-windows


# avx: the features the JIT enables on this machine, which fminus-hostinfo lists one a line, include AVX.
def jit_targets_avx():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        return False
    try:
        listing = subprocess.run([os.path.join(config.fminus_tools_dir, "fminus-hostinfo"), "--features"],
                                 capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return False
    return "avx" in listing.split()


if jit_targets_avx():
    config.available_features.add("avx")


# json, xml: this build has the JSON parser and its builtins (FMINUS_HAS_JSON 1 in fminus/Config.h), or the XML
# parser and its (FMINUS_HAS_XML 1).
def has_feature(macro):
    try:
        with open(os.path.join(config.fminus_obj_root, "include", "fminus", "Config.h")) as header:
            return any(line.split()[:3] == ["#define", macro, "1"] for line in header)
    except OSError:
        return False


if has_feature("FMINUS_HAS_JSON"):
    config.available_features.add("json")
if has_feature("FMINUS_HAS_XML"):
    config.available_features.add("xml")
