# tests/lit/lit.cfg.py: FMinus's lit suite. Run it through the build tree's bin/fminus-lit (or ctest), which loads the
# generated lit.site.cfg.py first; that file holds the paths read here.
#
# A test is a file with RUN lines (lit's internal shell: pipes, `not`, `env`; FileCheck checks the output). FMinus's
# tools, then LLVM's (FileCheck, not, count, opt, llc, llvm-mca, ...), come first on PATH. Substitutions: %host_triple.
# Features for REQUIRES/UNSUPPORTED: asserts, asan, ubsan, tsan, the host's architecture (arm64 or x86_64; Windows
# spells them ARM64 and AMD64) and its system (system-darwin, system-linux, system-windows).
import os
import platform

import lit.formats

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

config.substitutions.append(("%host_triple", config.host_triple))

if config.llvm_assertions:
    config.available_features.add("asserts")
for sanitizer, feature in (("address", "asan"), ("undefined", "ubsan"), ("thread", "tsan")):
    if sanitizer in config.sanitizers:
        config.available_features.add(feature)
config.available_features.add(platform.machine())
config.available_features.add("system-" + platform.system().lower())   # system-darwin, system-linux, system-windows
