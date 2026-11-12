"""The Python package's tests (docs/python.md): unittest's, so they need nothing installed. ctest runs them as the
test "python"; by hand, `PYTHONPATH=<build>/python python -m unittest discover -s python/tests -v`. The NumPy ones
are skipped where NumPy is not installed."""

import array
import contextlib
import ctypes
import gc
import io
import os
import runpy
import tempfile
import threading
import unittest

import fminus

try:
    import numpy
except ImportError:
    numpy = None


def floats(*values):
    return array.array("f", values)


FUNCTIONS = """
float twice(float x) { return 2 * x; }
double half(double x) { return x / 2; }
int add(int a, int b) { return a + b; }
bool both(bool a, bool b) { return a && b; }
void nothing() { }
void gain(const float* restrict in, float* restrict out, int n, float g)
{
    for (int i = 0; i < n; ++i)
        out[i] = g * in[i];
}
int sum(const int* values, int n)
{
    int total = 0;
    for (int i = 0; i < n; ++i)
        total += values[i];
    return total;
}
double mean(const double* values, int n)
{
    double total = 0;
    for (int i = 0; i < n; ++i)
        total += values[i];
    return total / n;
}
bool same(const void* a, const void* b) { return a == b; }
"""

MODULES = """
module Lowpass
{
    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]] [[ui::unit("Hz")]];
    param int order [[fminus::default(1)]];
    float state;
    float history[4];

    float tick(float x)
    {
        state += 0.5f * (x - state);
        return state;
    }

    float cutoff_now() { return cutoff[0]; }

    void fill(float* restrict out, int n)
    {
        for (int i = 0; i < n; ++i)
            out[i] = cutoff[i];
    }
};

float state_of(const Lowpass* filter) { return filter->state; }
"""


class Compile(unittest.TestCase):
    def test_version(self):
        self.assertRegex(fminus.__version__, r"^\d+\.\d+\.\d+$")
        self.assertRegex(fminus.llvm_version, r"^\d+\.")

    def test_error_is_clangs_words(self):
        with self.assertRaises(fminus.CompileError) as raised:
            fminus.compile("float f() { return x; }", file_name="synth.fm")
        self.assertEqual(str(raised.exception), "synth.fm:1:20: error: use of undeclared identifier 'x'")
        self.assertIsInstance(raised.exception, fminus.Error)

    def test_options(self):
        program = fminus.compile(FUNCTIONS, optimisation=0, math="libm", denormals="flush", threads=1,
                                 strict_aliasing=False, knob_versions=0, poly_lanes=4)
        self.assertEqual(program.twice(4), 8.0)
        self.assertEqual(program.poly_lanes, 4)
        for option in ("math", "denormals", "keep_ir"):
            with self.assertRaises(ValueError):
                fminus.compile(FUNCTIONS, **{option: "no such"})

    def test_keep_ir(self):
        self.assertEqual(fminus.compile(FUNCTIONS).optimised_ir, "")
        program = fminus.compile(FUNCTIONS, keep_ir="both")
        self.assertIn("define", program.lowered_ir)
        self.assertIn("@twice", program.optimised_ir)

    def test_warnings(self):
        program = fminus.compile("module M { float level [[range(0, 1)]]; };")
        self.assertIn("-Wunknown-attributes", program.warnings)
        self.assertEqual(fminus.compile(FUNCTIONS).warnings, "")

    def test_defines(self):
        source = """
            #ifdef DEBUG
            int level() { return 1; }
            #else
            int level() { return 0; }
            #endif
            #if LEVEL >= 2
            int high() { return 1; }
            #endif
        """
        self.assertEqual(fminus.compile(source).level(), 0)
        program = fminus.compile(source, defines={"DEBUG": True})   # a flag, as -D DEBUG
        self.assertEqual(program.level(), 1)
        self.assertFalse(hasattr(program, "high"))
        self.assertEqual(fminus.compile(source, defines={"LEVEL": 2}).high(), 1)
        with self.assertRaises(fminus.CompileError):
            fminus.compile(source, defines={"not a name": 1})


class Calls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.program = fminus.compile(FUNCTIONS)

    def test_scalars(self):
        program = self.program
        self.assertEqual(program.twice(21), 42.0)
        self.assertEqual(program.twice(0.1), 2 * ctypes.c_float(0.1).value)   # a float's: rounded to one
        self.assertEqual(program.half(0.1), 0.05)                             # a double's: Python's own
        self.assertEqual(program.add(2, 3), 5)
        self.assertIs(program.both(True, 1), True)
        self.assertIs(program.both(True, False), False)
        self.assertIsNone(program.nothing())

    def test_function(self):
        twice = self.program.function("twice")
        self.assertIsInstance(twice, fminus.Function)
        self.assertEqual(twice.name, "twice")
        self.assertEqual(twice.signature, "float twice(float x)")
        self.assertEqual(twice.result_type, "float")
        self.assertEqual([(p.name, p.type) for p in twice.parameters], [("x", "float")])
        self.assertIsNone(twice.module)
        self.assertFalse(twice.nonblocking)
        self.assertEqual(repr(twice), "<fminus.Function float twice(float x)>")
        self.assertEqual(self.program.gain.signature,
                         "void gain(const float *in, float *out, int n, float g)")

    def test_no_function(self):
        with self.assertRaisesRegex(AttributeError, "no function 'thrice'"):
            self.program.thrice
        with self.assertRaisesRegex(fminus.Error, "no function 'thrice'"):
            self.program.function("thrice")
        self.assertFalse(hasattr(self.program, "thrice"))

    def test_wrong_scalars(self):
        program = self.program
        with self.assertRaisesRegex(TypeError, "'twice' takes 1 argument, not 2"):
            program.twice(1, 2)
        with self.assertRaisesRegex(TypeError, "argument 1 of 'twice' is a 'str'"):
            program.twice("1")
        with self.assertRaisesRegex(TypeError, "argument 2 of 'add' is a 'float'"):
            program.add(1, 2.0)
        with self.assertRaisesRegex(OverflowError, "argument 1 of 'add' does not fit"):
            program.add(2**31, 0)
        with self.assertRaises(TypeError):
            program.both(True, "yes")
        with self.assertRaises(TypeError):
            program.twice(x=1)

    def test_buffers(self):
        program = self.program
        samples, out = floats(1, 2, 3), floats(0, 0, 0)
        self.assertIsNone(program.gain(samples, out, 3, 0.5))
        self.assertEqual(list(out), [0.5, 1.0, 1.5])
        program.gain(memoryview(samples), memoryview(out), 3, 2)
        self.assertEqual(list(out), [2.0, 4.0, 6.0])
        self.assertEqual(program.sum(array.array("i", [1, 2, 3]), 3), 6)
        self.assertEqual(program.mean(array.array("d", [1, 2]), 2), 1.5)
        self.assertEqual(program.sum((ctypes.c_int * 2)(4, 5), 2), 9)
        self.assertEqual(program.sum(None, 0), 0)   # None is a null pointer

    def test_void_pointer(self):
        program = self.program
        buffer = floats(1)
        self.assertIs(program.same(None, None), True)        # a void* takes any buffer, an instance, or None
        self.assertIs(program.same(buffer, buffer), True)
        self.assertIs(program.same(buffer, None), False)
        self.assertIs(program.same(b"bytes", buffer), False)

    def test_wrong_buffers(self):
        program = self.program
        samples, out = floats(1, 2, 3), floats(0, 0, 0)
        with self.assertRaisesRegex(TypeError, "argument 2 of 'gain' is 'double \\*', not 'float \\*'"):
            program.gain(samples, array.array("d", [0, 0, 0]), 3, 1)
        with self.assertRaisesRegex(TypeError, "argument 1 of 'gain' is a buffer of format 'B'"):
            program.gain(b"twelve bytes", out, 3, 1)
        with self.assertRaisesRegex(TypeError, "argument 2 of 'gain' is a read-only"):
            program.gain(samples, memoryview(out).toreadonly(), 3, 1)
        with self.assertRaisesRegex(TypeError, "argument 1 of 'gain' is a 'list'"):
            program.gain([1, 2, 3], out, 3, 1)
        self.assertEqual(list(out), [0, 0, 0])
        program.gain(memoryview(samples).toreadonly(), out, 3, 1)   # a const float* takes a read-only buffer
        self.assertEqual(list(out), [1, 2, 3])

    def test_address(self):
        twice = ctypes.CFUNCTYPE(ctypes.c_float, ctypes.c_float)(self.program.address("twice"))
        self.assertEqual(twice(1.5), 3.0)
        with self.assertRaises(fminus.Error):
            self.program.address("thrice")

    def test_a_function_keeps_its_program(self):
        twice = fminus.compile(FUNCTIONS).twice
        gc.collect()
        self.assertEqual(twice(2), 4.0)

    def test_threads(self):
        # the GIL is released while a function runs: calls from several threads at once
        program, results = self.program, []
        samples = floats(*range(1000))

        def run():
            out = floats(*([0] * 1000))
            for _ in range(200):
                program.gain(samples, out, 1000, 2)
            results.append(out[999])

        threads = [threading.Thread(target=run) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(results, [1998.0] * 4)

    @unittest.skipIf(numpy is None, "NumPy is not installed")
    def test_numpy(self):
        program = self.program
        samples = numpy.arange(8, dtype=numpy.float32)
        out = numpy.zeros(8, dtype=numpy.float32)
        program.gain(samples, out, len(samples), numpy.float32(0.5))
        numpy.testing.assert_array_equal(out, samples / 2)
        self.assertEqual(program.sum(numpy.arange(4, dtype=numpy.int32), numpy.int64(4)), 6)
        self.assertEqual(program.mean(numpy.array([1.0, 3.0]), 2), 2.0)
        self.assertIs(program.both(numpy.bool_(True), True), True)
        with self.assertRaisesRegex(TypeError, "'double \\*', not 'const float \\*'"):
            program.gain(numpy.arange(8.0), out, 8, 1)       # float64 is no float
        with self.assertRaisesRegex(TypeError, "not one block of memory"):
            program.gain(samples[::2], out, 4, 1)            # a strided view
        samples.flags.writeable = False
        with self.assertRaisesRegex(TypeError, "read-only"):
            program.gain(out, samples, 8, 1)


class Modules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.program = fminus.compile(MODULES)

    def test_description(self):
        module = self.program.module("Lowpass")
        self.assertIsInstance(module, fminus.Module)
        self.assertEqual([m.name for m in self.program.modules], ["Lowpass"])
        self.assertEqual(module.name, "Lowpass")
        self.assertEqual(module.methods, ["Lowpass::tick", "Lowpass::cutoff_now", "Lowpass::fill"])
        self.assertEqual([f.name for f in module.fields], ["cutoff", "order", "state", "history"])
        history = module.fields[3]
        self.assertEqual((history.type, history.element, history.extents, history.size),
                         ("float[4]", "float", (4,), 16))
        self.assertLessEqual(history.offset + history.size, module.size)
        cutoff = module.parameters[0]
        self.assertEqual((cutoff.module, cutoff.name, cutoff.type, cutoff.binding), ("Lowpass", "cutoff", "float",
                                                                                     "knob"))
        self.assertEqual((cutoff.minimum, cutoff.maximum, cutoff.default), (20.0, 20000.0, 1000.0))
        self.assertEqual([(a.name, a.arguments) for a in cutoff.attributes],
                         [("fminus::range", [20.0, 20000.0]), ("fminus::default", [1000.0]), ("ui::unit", ["Hz"])])
        with self.assertRaisesRegex(fminus.Error, "no module 'Highpass'"):
            self.program.module("Highpass")

    def test_methods(self):
        lowpass = self.program.module("Lowpass").instance()
        self.assertIsInstance(lowpass, fminus.Instance)
        self.assertEqual(lowpass.tick(1), 0.5)
        self.assertEqual(lowpass.tick(1), 0.75)      # its state stays in the instance
        self.assertEqual(lowpass.tick.signature, "float Lowpass::tick(float x)")
        self.assertIs(lowpass.tick.instance, lowpass)
        self.assertEqual(lowpass.tick.module, "Lowpass")
        other = self.program.module("Lowpass")()     # a module called makes an instance too
        self.assertEqual(other.tick(1), 0.5)
        with self.assertRaisesRegex(AttributeError, "no function 'Lowpass::tock'"):
            lowpass.tock

    def test_unbound(self):
        lowpass = self.program.module("Lowpass").instance()
        tick = self.program.function("Lowpass::tick")
        self.assertEqual(tick.signature, "float Lowpass::tick(Lowpass *this, float x)")
        self.assertEqual(tick(lowpass, 1), 0.5)
        self.assertEqual(self.program.state_of(lowpass), 0.5)
        with self.assertRaisesRegex(TypeError, "a method runs on an instance"):
            tick(None, 1)
        with self.assertRaises(TypeError):
            tick(floats(0, 0, 0, 0, 0, 0, 0, 0), 1)   # floats are no Lowpass
        with self.assertRaisesRegex(TypeError, "another program's module"):
            tick(fminus.compile(MODULES).module("Lowpass").instance(), 1)

    def test_parameters(self):
        lowpass = self.program.module("Lowpass").instance()
        self.assertEqual(lowpass["cutoff"], 1000.0)   # its fminus::default
        self.assertEqual(lowpass.get("order"), 1.0)
        lowpass["cutoff"] = 440
        self.assertEqual(lowpass.cutoff_now(), 440.0)
        lowpass.set("cutoff", 880)
        self.assertEqual(lowpass.cutoff_now(), 880.0)
        lowpass.set_defaults()
        self.assertEqual(lowpass.cutoff_now(), 1000.0)
        with self.assertRaisesRegex(KeyError, "no parameter 'resonance'"):
            lowpass["resonance"] = 1
        with self.assertRaisesRegex(fminus.Error, "'Lowpass::cutoff' is no stream"):
            lowpass.set_stream("cutoff", floats(1, 2))

    def test_reset(self):
        lowpass = self.program.module("Lowpass").instance()
        lowpass["cutoff"] = 440
        lowpass.tick(1)
        lowpass.reset()
        self.assertEqual(lowpass["cutoff"], 1000.0)
        self.assertEqual(lowpass.tick(1), 0.5)

    def test_memory(self):
        module = self.program.module("Lowpass")
        lowpass = module.instance()
        view = memoryview(lowpass)
        self.assertEqual((view.nbytes, view.readonly), (module.size, False))
        lowpass.tick(1)
        state = next(f for f in module.fields if f.name == "state")
        self.assertEqual(view[state.offset:state.offset + state.size].cast("f")[0], 0.5)
        view[state.offset:state.offset + state.size].cast("f")[0] = 0   # the instance's own memory: writable
        self.assertEqual(lowpass.tick(1), 0.5)

    def test_stream(self):
        program = fminus.compile(MODULES, parameters={"Lowpass::cutoff": "stream"})
        self.assertEqual(program.module("Lowpass").parameters[0].binding, "stream")
        lowpass = program.module("Lowpass").instance()
        out = floats(0, 0, 0)
        lowpass.set_stream("cutoff", floats(100, 200, 300))   # the instance holds the samples
        gc.collect()
        lowpass.fill(out, 3)
        self.assertEqual(list(out), [100, 200, 300])
        lowpass.set_stream("cutoff", None)                    # flat: every sample is the value
        lowpass["cutoff"] = 50
        lowpass.fill(out, 3)
        self.assertEqual(list(out), [50, 50, 50])
        with self.assertRaises(TypeError):
            lowpass.set_stream("cutoff", array.array("d", [1, 2, 3]))

    def test_frozen(self):
        program = fminus.compile(MODULES, parameters={"Lowpass::cutoff": 250})
        cutoff = program.module("Lowpass").parameters[0]
        self.assertEqual((cutoff.binding, cutoff.frozen), ("frozen", 250.0))
        lowpass = program.module("Lowpass").instance()
        lowpass["cutoff"] = 440   # a frozen parameter's methods do not read it
        self.assertEqual(lowpass.cutoff_now(), 250.0)
        with self.assertRaises(fminus.CompileError):
            fminus.compile(MODULES, parameters={"Lowpass::resonance": "knob"})
        with self.assertRaises(ValueError):
            fminus.compile(MODULES, parameters={"Lowpass::cutoff": "lfo"})


class Safety(unittest.TestCase):
    def test_loop_budget(self):
        program = fminus.compile("float settle(float x) { while (x > 1) x = x * 0.5f; return x; }",
                                 loop_budget=True)
        program.set_loop_budget(1000)
        self.assertEqual(program.settle(8), 1.0)
        self.assertFalse(program.loop_budget_exceeded)
        program.set_loop_budget(1000)
        with self.assertRaisesRegex(fminus.LoopBudgetError, "'settle' ran past its loop budget"):
            program.settle(float("inf"))   # half of infinity is infinity: it never ends
        self.assertTrue(program.loop_budget_exceeded)
        self.assertEqual(program.loop_budget_left, -1)
        program.set_loop_budget(1000)
        self.assertEqual(program.settle(4), 1.0)

    def test_bounds_check(self):
        source = "float tap(int i) { float taps[16]; taps[i] = 1; return taps[i]; }"
        program = fminus.compile(source, bounds_check=True, file_name="taps.fm")
        self.assertEqual(program.tap(3), 1.0)
        self.assertIsNone(program.bounds_violation)
        with self.assertRaises(fminus.BoundsError) as raised:
            program.tap(20)
        self.assertIn("index 20 out of bounds for 'taps' of type 'float[16]'", str(raised.exception))
        violation = program.bounds_violation
        self.assertEqual((violation.file, violation.line, violation.array, violation.index, violation.extent),
                         ("taps.fm", 1, "taps", 20, 16))
        self.assertIn(violation.message, str(raised.exception))
        with self.assertRaises(fminus.BoundsError):   # the record holds the check until it is cleared
            program.tap(3)
        program.clear_bounds_violation()
        self.assertEqual(program.tap(3), 1.0)

    def test_code_ranges(self):
        program = fminus.compile(FUNCTIONS)
        ranges = program.code_ranges()
        self.assertIn("twice", [r.function for r in ranges if r.kind == "function"])
        self.assertEqual(sorted(r.address for r in ranges), [r.address for r in ranges])
        twice = next(r for r in ranges if r.symbol == "twice")
        self.assertTrue(twice.address <= program.address("twice") < twice.address + twice.size)


class Includes(unittest.TestCase):
    MAIN = '#include "gain.fm"\nfloat run(float x) { return gain(x); }\n'

    def test_no_resolver(self):
        with self.assertRaisesRegex(fminus.CompileError, "'gain.fm' file not found"):
            fminus.compile(self.MAIN)

    def test_resolver(self):
        asked = []

        def include(name, including):
            asked.append((name, including))
            return "float gain(float x) { return 3 * x; }" if name == "gain.fm" else None

        self.assertEqual(fminus.compile(self.MAIN, include=include, file_name="main.fm").run(2), 6.0)
        self.assertEqual(asked, [("gain.fm", "main.fm")])

    def test_resolver_names_the_file(self):
        with self.assertRaisesRegex(fminus.CompileError, "\nlib/gain.fm:1:30: error"):
            fminus.compile(self.MAIN,
                           include=lambda name, including: ("lib/gain.fm", "float gain(float x) { return y; }"))

    def test_resolver_raises(self):
        def include(name, including):
            raise LookupError(name)

        with self.assertRaisesRegex(LookupError, "gain.fm"):
            fminus.compile(self.MAIN, include=include)
        with self.assertRaises(TypeError):
            fminus.compile(self.MAIN, include=lambda name, including: 42)

    def test_load(self):
        with tempfile.TemporaryDirectory() as directory:
            os.mkdir(os.path.join(directory, "lib"))
            os.mkdir(os.path.join(directory, "elsewhere"))
            main = os.path.join(directory, "main.fm")
            with open(main, "w", encoding="utf-8") as file:
                file.write('#include "lib/gain.fm"\n#include "offset.fm"\n'
                           "float run(float x) { return offset(gain(x)); }\n")
            with open(os.path.join(directory, "lib", "gain.fm"), "w", encoding="utf-8") as file:
                file.write('#include "../main.fm"\nfloat gain(float x) { return 3 * x; }\n')   # read once
            with open(os.path.join(directory, "elsewhere", "offset.fm"), "w", encoding="utf-8") as file:
                file.write("float offset(float x) { return x + 1; }\n")
            with self.assertRaisesRegex(fminus.CompileError, "'offset.fm' file not found"):
                fminus.load(main)
            program = fminus.load(main, include_dirs=[os.path.join(directory, "elsewhere")])
            self.assertEqual(program.run(2), 7.0)
            program = fminus.load(main, include=lambda name, including: "float offset(float x) { return x; }"
                                  if name == "offset.fm" else None)
            self.assertEqual(program.run(2), 6.0)
            with open(main, "a", encoding="utf-8") as file:
                file.write("float broken() { return y; }\n")
            with self.assertRaises(fminus.CompileError) as raised:
                fminus.load(main, include_dirs=[os.path.join(directory, "elsewhere")])
            self.assertTrue(str(raised.exception).startswith(main + ":4:"))
        with self.assertRaises(FileNotFoundError):
            fminus.load(os.path.join(directory, "main.fm"))

    def test_load_an_example(self):
        # FMINUS_EXAMPLES_DIR (ctest sets it), or the checkout's examples/ beside python/
        examples = os.environ.get("FMINUS_EXAMPLES_DIR") or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, "examples")
        if not os.path.isdir(examples):
            self.skipTest("no examples/ directory (FMINUS_EXAMPLES_DIR names it)")
        program = fminus.load(os.path.join(examples, "parameters", "parameters.fm"))
        lfo, filter_ = program.module("Lfo").instance(), program.module("Filter").instance()
        self.assertTrue(lfo.process.nonblocking)
        cutoff, out = floats(*([0] * 64)), floats(*([0] * 64))
        lfo.process(cutoff, 64)
        self.assertEqual(cutoff[0], 800.0)   # its centre's default, at phase 0
        filter_.process(floats(*([1] * 64)), out, 64)
        self.assertTrue(0 < out[63] <= 2)


class Example(unittest.TestCase):
    def test_sweep(self):
        # python/examples/sweep.py: an LFO's stream sweeps a filter's cutoff, a block a call
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "examples", "sweep.py")
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            peaks = runpy.run_path(path)["main"]()
        self.assertEqual(len(printed.getvalue().splitlines()), 8)
        self.assertTrue(all(0 < peak < 4 for peak in peaks))
        self.assertGreater(max(peaks), 2 * min(peaks))   # the sweep is heard


if __name__ == "__main__":
    unittest.main()
