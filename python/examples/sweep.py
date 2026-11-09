"""python/examples/sweep.py: an FMinus program run from Python (docs/python.md). It loads examples/parameters/
parameters.fm, a state-variable filter and an LFO for its cutoff, compiles the cutoff as a stream, a value a sample,
and filters a sawtooth a block at a time while the LFO sweeps it, printing each block's peak.

    PYTHONPATH=build-python/python python3 python/examples/sweep.py
"""

import array
import os

import fminus

BLOCK = 256
SOURCE = os.path.join(os.path.dirname(__file__), "..", "..", "examples", "parameters", "parameters.fm")


def main():
    # How a parameter reads is the host's choice when it compiles: the filter's cutoff a stream, the rest knobs.
    program = fminus.load(SOURCE, parameters={"Filter::cutoff": "stream"})

    lfo = program.module("Lfo").instance()         # zeroed, its parameters at their defaults
    lfo["frequency"] = 40                          # Hz: fast, so that a few blocks show the sweep
    filter_ = program.module("Filter").instance()
    filter_["damping"] = 0.2                       # a resonant low-pass

    saw = array.array("f", [2 * (i % 64) / 64 - 1 for i in range(BLOCK)])   # 750 Hz at 48 kHz
    cutoff = array.array("f", bytes(4 * BLOCK))
    out = array.array("f", bytes(4 * BLOCK))
    filter_.set_stream("cutoff", cutoff)           # the filter reads its cutoff from this buffer

    peaks = []
    for block in range(8):
        lfo.process(cutoff, BLOCK)                 # the LFO writes a cutoff a sample
        filter_.process(saw, out, BLOCK)           # one call a block: the loop is FMinus's
        peaks.append(max(abs(sample) for sample in out))
        print(f"block {block}: cutoff {cutoff[0]:7.1f} to {cutoff[-1]:7.1f} Hz, peak {peaks[-1]:.3f}")
    return peaks


if __name__ == "__main__":
    main()
