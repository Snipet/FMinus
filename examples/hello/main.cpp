// examples/hello/main.cpp: FMinus as a library, at its smallest. A C++ main JIT-compiles an FMinus program from a
// string and calls one of its functions (README.md, "Using FMinus as a library").
#include "fminus/FMinus.h"

#include <cstdio>
#include <string>

int main()
{
    std::string errors;
    auto program = fminus::Program::compile("float twice(float x) { return 2 * x; }", errors);
    if (!program)
    {
        std::fputs(errors.c_str(), stderr);   // FMinus's errors, as clang words them
        return 1;
    }
    auto* twice = program->function<float(float)>("twice", errors);
    if (!twice)
    {
        std::fputs(errors.c_str(), stderr);
        return 1;
    }
    const float result = twice(21.0f);   // a plain C++ call into the JIT'd code
    std::printf("twice(21) = %g\n", static_cast<double>(result));
    return result == 42.0f ? 0 : 1;
}
