// examples/circuits/main.cpp: example-circuits, the circuit compiler's example (docs/circuits.md). Its netlists are
// the report's (docs/research/analog-circuits/README.md): diodetone.cir (example 1), screamer.cir (example 2, with
// §9.1's tone-pot line), screamer-clip.cir (its clipping stage alone) and clipper.cir (Yeh's RC clipper).
//
//     build-dev/bin/example-circuits --check          the targets, T1-T4, and the rest (what ctest runs; nothing timed)
//     build-dev/bin/example-circuits --tables         the report's accuracy tables from emitted code (minutes)
//     build-dev/bin/example-circuits                  the report's cost table from emitted code (on a quiet machine)
//     build-dev/bin/example-circuits --all --only adaa --math fast --reps 7 --samples 1000000
#include "harness.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <exception>
#include <string>

int main(int argc, char** argv)
{
    CLI::App app{"The circuit compiler's example: generated modules checked against the report's targets, its "
                 "accuracy tables and its cost table",
                 "example-circuits"};
    bool check = false;
    bool tables = false;
    circuits::BenchOptions bench;
    std::string math = "vector";
    app.add_flag("--check", check, "Check the targets and the rest (ctest's run); nothing is timed");
    app.add_flag("--tables", tables, "Print the report's accuracy tables, from emitted code (minutes)");
    app.add_flag("--all", bench.all, "The cost table: mkbench.py's whole matrix");
    app.add_option("--only", bench.only, "The cost table: the rows whose name matches this regex");
    app.add_option("--math", math, "The cost table's math mode: vector, fast or libm")
        ->check(CLI::IsMember({"vector", "fast", "libm"}))
        ->capture_default_str();
    app.add_option("--reps", bench.reps, "The cost table: the best of this many runs")->capture_default_str();
    app.add_option("--samples", bench.samples, "The cost table: host samples a run")->capture_default_str();
    CLI11_PARSE(app, argc, argv);
    bench.math = math == "fast"   ? fminus::Program::Math::Fast
                 : math == "libm" ? fminus::Program::Math::Libm
                                  : fminus::Program::Math::Vector;
    try
    {
        if (check)
            return circuits::check();
        if (tables)
            return circuits::tables();
        return circuits::bench(bench);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "example-circuits: %s\n", e.what());
        return 1;
    }
}
