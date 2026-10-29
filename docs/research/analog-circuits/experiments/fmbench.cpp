// fmbench: compile an FMinus file with FMinus c75caab (the library the playground pins) and time float f(float n)
// entry points in-process: wall time, and this process's cycles and instructions (proc_pid_rusage, RUSAGE_INFO_V4,
// as FMinus's recipe 24 measured). Usage: fmbench file.fm vector|fast|libm n reps fn1 [fn2 ...]
#include "fminus/FMinus.h"

#include <libproc.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static void counters(uint64_t& cycles, uint64_t& instr)
{
    rusage_info_v4 ri{};
    proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri));
    cycles = ri.ri_cycles;
    instr = ri.ri_instructions;
}

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::fprintf(stderr, "usage: fmbench file.fm vector|fast|libm n reps fn...\n");
        return 2;
    }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // prefer a performance core
    std::ifstream f(argv[1]);
    std::stringstream ss;
    ss << f.rdbuf();
    fminus::Program::Options o;
    o.fileName = argv[1];
    o.math = std::strcmp(argv[2], "fast") == 0   ? fminus::Program::Math::Fast
             : std::strcmp(argv[2], "libm") == 0 ? fminus::Program::Math::Libm
                                                 : fminus::Program::Math::Vector;
    o.denormals = fminus::Program::Denormals::Flush;
    o.threads = 1;
    const float n = float(std::atof(argv[3]));
    const int reps = std::atoi(argv[4]);
    std::string errors;
    const auto t0 = std::chrono::steady_clock::now();
    auto program = fminus::Program::compile(ss.str(), errors, o);
    const auto t1 = std::chrono::steady_clock::now();
    if (!program)
    {
        std::fprintf(stderr, "%s", errors.c_str());
        return 1;
    }
    std::printf("compile %.1f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());
    for (int a = 5; a < argc; ++a)
    {
        const auto l0 = std::chrono::steady_clock::now();
        auto* fn = program->function<float(float)>(argv[a], errors);
        const auto l1 = std::chrono::steady_clock::now();
        if (!fn)
        {
            std::fprintf(stderr, "%s", errors.c_str());
            return 1;
        }
        volatile float sink = fn(1000);   // warm
        double bestNs = 1e30, bestCyc = 1e30, bestIns = 1e30;
        float result = 0;
        for (int r = 0; r < reps; ++r)
        {
            uint64_t c0, i0, c1, i1;
            counters(c0, i0);
            const auto w0 = std::chrono::steady_clock::now();
            result = fn(n);
            const auto w1 = std::chrono::steady_clock::now();
            counters(c1, i1);
            bestNs = std::min(bestNs, std::chrono::duration<double, std::nano>(w1 - w0).count() / n);
            bestCyc = std::min(bestCyc, double(c1 - c0) / n);
            bestIns = std::min(bestIns, double(i1 - i0) / n);
        }
        sink = result;
        (void)sink;
        std::printf("%-28s %8.2f ns  %8.1f cycles  %8.1f instr  per call-unit   (lookup %.1f ms, result %g)\n", argv[a], bestNs,
                    bestCyc, bestIns, std::chrono::duration<double, std::milli>(l1 - l0).count(), double(result));
        std::fflush(stdout);
    }
    return 0;
}
