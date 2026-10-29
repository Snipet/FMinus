// src/circuit/Runners.cpp: the runners' FMinus text (Runners.h).
#include "circuit/Runners.h"

namespace fminus::circuit
{
    namespace
    {
        // "pots_[i * P + 0], ..., switches_[i * S + 0], ..." (`at`: "i" or "0")
        std::string setArguments(const RunnerShape& s, const std::string& at)
        {
            std::string args;
            for (int k = 0; k < s.pots; ++k)
                args += (args.empty() ? "" : ", ") + std::string("pots_[") + at + " * " + std::to_string(s.pots) +
                        " + " + std::to_string(k) + "]";
            for (int k = 0; k < s.switches; ++k)
                args += (args.empty() ? "" : ", ") + std::string("switches_[") + at + " * " +
                        std::to_string(s.switches) + " + " + std::to_string(k) + "]";
            return args;
        }

        // ", law_[i * L + 0], ..."
        std::string lawArguments(const RunnerShape& s)
        {
            std::string args;
            for (int k = 0; k < s.laws; ++k)
                args += ", law_[i * " + std::to_string(s.laws) + " + " + std::to_string(k) + "]";
            return args;
        }
    } // namespace

    std::string runnerSource(const RunnerShape& s)
    {
        const std::string& m = s.module;
        const bool sets = s.pots + s.switches > 0;
        const std::string laws = lawArguments(s);
        std::string out = "\n// fminus::circuit's runners (src/circuit/Runners.h): a buffer a call.\n";
        out += "void fm_set(" + m +
               "* c_, const float* restrict pots_, const int* restrict switches_) [[clang::nonblocking]]\n{\n";
        if (sets)
            out += "    c_->set(" + setArguments(s, "0") + ");\n";
        out += "}\n";
        const std::string set =
            sets ? "        if ((start_ + i) % 16 == 0)\n            c_->set(" + setArguments(s, "i") + ");\n" : "";
        if (s.channels == 1)
        {
            if (s.step)
                out += "void fm_steps(" + m +
                       "* c_, const float* restrict u_, const float* restrict law_, float* restrict y_, int n_) "
                       "[[clang::nonblocking]]\n{\n    for (int i = 0; i < n_; ++i)\n        y_[i] = c_->step(u_[i]" +
                       laws + ");\n}\n";
            out += "void fm_ticks(" + m +
                   "* c_, const float* restrict u_, const float* restrict law_, const float* restrict pots_, "
                   "const int* restrict switches_, float* restrict y_, int n_, int start_) [[clang::nonblocking]]\n"
                   "{\n    for (int i = 0; i < n_; ++i)\n    {\n" +
                   set + "        y_[i] = c_->tick(u_[i]" + laws + ");\n    }\n}\n";
            return out;
        }
        out += "void fm_ticks2(" + m +
               "* c_, const float* restrict a_, const float* restrict b_, const float* restrict law_, "
               "const float* restrict pots_, const int* restrict switches_, float* restrict ya_, float* restrict yb_, "
               "int n_, int start_) [[clang::nonblocking]]\n{\n    for (int i = 0; i < n_; ++i)\n    {\n" +
               set + "        const float2 y = c_->tick((float2)(a_[i], b_[i])" + laws +
               ");\n        ya_[i] = y.x;\n        yb_[i] = y.y;\n    }\n}\n";
        return out;
    }
} // namespace fminus::circuit
