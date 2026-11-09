"""FMinus, a JIT compiler for audio DSP, as a Python package.

    import fminus

    program = fminus.compile("float twice(float x) { return 2 * x; }")
    program.twice(21)                               # 42.0

    program = fminus.load("examples/delay/delay.fm")
    program.function("feedback_delay")              # <fminus.Function void feedback_delay(...)>

compile() takes FMinus source, load() a file; each gives a Program, compiled for this machine, or raises
CompileError with FMinus's messages, as clang words them. A Program's functions are its attributes, and are called
with Python's numbers and, for a pointer, with a buffer: an array.array, a NumPy array, a memoryview. Its modules
make instances, whose methods are attributes too, and whose parameters are items:

    lowpass = program.module("Lowpass").instance()
    lowpass["cutoff"] = 1000
    lowpass.process(samples, out, len(samples))

docs/python.md is the manual.
"""

import os as _os

from ._fminus import (
    BoundsError,
    CompileError,
    Error,
    Function,
    Instance,
    LoopBudgetError,
    Module,
    Program,
    __version__,
    _compile,
    llvm_version,
)

__all__ = [
    "BoundsError",
    "CompileError",
    "Error",
    "Function",
    "Instance",
    "LoopBudgetError",
    "Module",
    "Program",
    "compile",
    "llvm_version",
    "load",
]


class _Includes:
    """Where #include "name" finds its file: the caller's `include` first, then the disk, as the fminus driver looks:
    beside the including file, then in each of `directories` in turn (an absolute name is itself). A file on disk is
    named as found, except one read already by another name, which keeps its first: the compiler reads each file
    once, by name."""

    def __init__(self, include, directories, file_name):
        self._include = include
        self._directories = [_os.fspath(directory) for directory in directories]
        self._names = {}
        if _os.path.isfile(file_name):
            self._remember(file_name)   # the program's own file is read already

    def __call__(self, name, including):
        if self._include is not None:
            found = self._include(name, including)
            if found is not None:
                if isinstance(found, str):   # the text alone: the file is named as the #include names it
                    return (name, found)
                found_name, source = found
                return (_os.fspath(found_name), source)
        if _os.path.isabs(name):
            candidates = [name]
        else:
            candidates = [_os.path.join(_os.path.dirname(including), name)]
            candidates += [_os.path.join(directory, name) for directory in self._directories]
        for candidate in candidates:
            candidate = _os.path.normpath(candidate)
            if not _os.path.isfile(candidate):
                continue
            try:
                with open(candidate, "rb") as file:
                    source = file.read().decode("utf-8")
            except OSError:
                continue
            return (self._remember(candidate), source)
        return None

    def _remember(self, path):
        return self._names.setdefault(_os.path.realpath(path), path)


def _define(value):
    if value is True:   # a flag, as -D NAME gives it
        return "1"
    if value is None or value is False:
        return ""
    return str(value)


def compile(
    source,
    *,
    file_name="program.fm",
    optimisation=2,
    poly_lanes=0,
    math="vector",
    include=None,
    include_dirs=(),
    defines=None,
    parameters=None,
    strict_aliasing=True,
    knob_versions=8,
    denormals="preserve",
    threads=0,
    keep_ir="none",
    loop_budget=False,
    bounds_check=False,
    _disk=False,
):
    """Compiles FMinus source, a str, for this machine, and returns its Program. Raises CompileError when it does
    not compile: its message is FMinus's errors, one a line (`program.fm:3:12: error: use of undeclared identifier
    'x'`). A program that compiles with warnings has them in Program.warnings.

    The keyword arguments are fminus::Program::Options (include/fminus/FMinus.h says what each costs):

    file_name        what the error messages call the source
    optimisation     0 to 3: LLVM's pipeline at that level, as fminus -O<n>
    poly_lanes       a poly float's lanes (1, 2, 4, ... 64); 0 is this machine's
    math             how sinf, cosf, ... compute: "vector" (FMinus's kernels, the same bits everywhere), "libm" or
                     "fast"
    include          where #include "name" finds a file the disk does not have: a function of the name and the
                     including file's name that returns the file's text, or (file_name, text), or None for no file
    include_dirs     directories #include looks in, after the including file's own
    defines          macros for the program's #if and #ifdef, a dict: {"DEBUG": True, "LEVEL": 2}
    parameters       how each module parameter reads, by its qualified name: {"Lowpass::cutoff": "stream"}; "knob"
                     (the default), "stream", or a number, the value it is frozen at
    strict_aliasing  whether the optimiser relies on C's strict-aliasing rule
    knob_versions    the most copies of a method knob versioning makes; 0 turns it off
    denormals        "preserve" (IEEE's subnormals) or "flush" (flushed to zero)
    threads          the most threads a parallel region runs on; 0 is every core, 1 the calling thread alone
    keep_ir          which LLVM IR to keep as text: "none", "lowered", "optimised" or "both" (Program.lowered_ir,
                     Program.optimised_ir)
    loop_budget      count loops against a budget (Program.set_loop_budget): a call that runs out returns early,
                     and raises LoopBudgetError
    bounds_check     check each access against its array: one outside stops the call, and raises BoundsError
    """
    file_name = _os.fspath(file_name)
    resolver = None
    if include is not None or include_dirs or _disk:   # _disk: load()'s, which reads #includes from disk
        resolver = _Includes(include, include_dirs, file_name)
    return _compile(
        source,
        file_name,
        optimisation,
        poly_lanes,
        math,
        resolver,
        {name: _define(value) for name, value in (defines or {}).items()},
        dict(parameters or {}),
        strict_aliasing,
        knob_versions,
        denormals,
        threads,
        keep_ir,
        loop_budget,
        bounds_check,
    )


def load(path, **options):
    """Compiles the FMinus program in the file `path` (UTF-8), and returns its Program. Its #includes are read from
    disk: beside the including file, then in include_dirs. The keyword arguments are compile()'s; file_name is the
    path unless given."""
    path = _os.fspath(path)
    with open(path, "rb") as file:
        source = file.read().decode("utf-8")
    options.setdefault("file_name", path)
    return compile(source, _disk=True, **options)
