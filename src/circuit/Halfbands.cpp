// src/circuit/Halfbands.cpp: the halfband stages (Halfbands.h). The coefficients are hb-coefs.txt's, as gen.cpp
// printed them (9 significant digits); the responses are gen.cpp's formulas.
#include "circuit/Halfbands.h"

#include "circuit/Netlist.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>

namespace fminus::circuit
{
    namespace
    {
        constexpr std::array kFir1{0.634741872,     -0.206632091,   0.118226751,     -0.0786035749,  0.055513298,
                                   -0.0402023258,   0.0293196828,   -0.0212981172,   0.0152910721,   -0.0107834672,
                                   0.00742785339,   -0.0049690905,  0.00320792632,   -0.00198296553, 0.0011616226,
                                   -0.000635394706, 0.000317030812, -0.000138337492, 4.80159027e-05, -9.30924486e-06};
        constexpr std::array kFir2{0.620817072,   -0.168807173,   0.0664050228,  -0.0241119407,
                                   0.00684006745, -0.00120247909, 4.47798767e-05};
        constexpr std::array kFir3{0.604044908, -0.130745117, 0.0307403378, -0.0040923127, 6.46820442e-05};
        constexpr std::array kIir1{0.0441819091, 0.16418886, 0.330744162, 0.515086943, 0.702052852, 0.894889642};
        constexpr std::array kIir2{0.0637878368, 0.266780962, 0.668548793};
        constexpr std::array kIir3{0.11030121, 0.536772124};

        void checkStage(int stage)
        {
            if (stage < 1 || stage > 3)
                throw std::out_of_range("halfband stage " + std::to_string(stage));
        }

        std::string rateText(int stage)
        {
            const int low = 48 << (stage - 1);
            return std::to_string(low) + " <-> " + std::to_string(2 * low) + " kHz";
        }

        std::string pre(int stage, char direction)
        {
            return "hb" + std::to_string(stage) + direction;
        }
    } // namespace

    std::span<const double> firTaps(int stage)
    {
        checkStage(stage);
        if (stage == 1)
            return kFir1;
        if (stage == 2)
            return kFir2;
        return kFir3;
    }

    std::span<const double> iirCoefficients(int stage)
    {
        checkStage(stage);
        if (stage == 1)
            return kIir1;
        if (stage == 2)
            return kIir2;
        return kIir3;
    }

    std::complex<double> iirResponse(int stage, double w)
    {
        const std::span<const double> a = iirCoefficients(stage);
        const std::complex<double> z2 = std::polar(1.0, -2 * w);
        std::complex<double> a0 = 1;
        std::complex<double> a1 = 1;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const std::complex<double> s = (a[i] + z2) / (1.0 + a[i] * z2);
            if (i % 2 == 0)
                a0 *= s;
            else
                a1 *= s;
        }
        return 0.5 * (a0 + std::polar(1.0, -w) * a1);
    }

    double firResponse(int stage, double w)
    {
        const std::span<const double> h2 = firTaps(stage);
        double re = 0.5;
        for (std::size_t j = 0; j < h2.size(); ++j)
            re += h2[j] * std::cos(w * static_cast<double>(2 * j + 1));
        return re;
    }

    double roundTripDelay(int oversample, Halfband kind, double freq)
    {
        const int stages = oversample == 8 ? 3 : oversample == 4 ? 2 : oversample == 2 ? 1 : 0;
        double total = 0;
        for (int s = 1; s <= stages; ++s)
        {
            const double factor = std::pow(2.0, s);   // high-rate samples per host sample
            if (kind == Halfband::Fir)
            {
                // each direction delays 2J high-rate samples (the centre tap)
                total += 2 * (2 * static_cast<double>(firTaps(s).size())) / factor;
                continue;
            }
            const double w = 2 * std::numbers::pi * freq / (48000 * factor);
            const double dw = 1e-6;
            double d = std::arg(iirResponse(s, w + dw)) - std::arg(iirResponse(s, w - dw));
            while (d > std::numbers::pi)
                d -= 2 * std::numbers::pi;
            while (d < -std::numbers::pi)
                d += 2 * std::numbers::pi;
            const double tau = -d / (2 * dw);   // high-rate samples
            // up: the polyphase interpolator is the filter on the zero-stuffed input; down: the decimator keeps the
            // filter's odd outputs, one high-rate sample early
            total += (tau + (tau - 1)) / factor;
        }
        return total;
    }

    namespace
    {
        // A field's declaration, its comment at the column the module's other fields use.
        std::string field(const std::string& declaration, const std::string& note = {})
        {
            std::string line = "    " + declaration;
            if (!note.empty())
            {
                line.append(line.size() < 31 ? 31 - line.size() : 2, ' ');
                line += "// " + note;
            }
            return line + "\n";
        }
    } // namespace

    void emitHalfbandFields(std::string& out, int stage, Halfband kind, const std::string& type)
    {
        const std::string up = pre(stage, 'u');
        const std::string down = pre(stage, 'd');
        if (kind == Halfband::Iir)
        {
            const std::string count = std::to_string(iirCoefficients(stage).size());
            out += field(type + " " + up + "_x[" + count + "];",
                         rateText(stage) + ", up (IIR, " + count + " allpass coefficients)");
            out += field(type + " " + up + "_y[" + count + "];");
            out += field(type + " " + down + "_x[" + count + "];", "and down");
            out += field(type + " " + down + "_y[" + count + "];");
            return;
        }
        const std::size_t j = firTaps(stage).size();
        const std::size_t w = 2 * j;
        out += field(type + " " + up + "_d[" + std::to_string(2 * w) + "];",
                     rateText(stage) + ", up (FIR, " + std::to_string(4 * j - 1) + " taps): a doubled ring");
        out += field("int " + up + "_pos;");
        out += field(type + " " + down + "_e[" + std::to_string(2 * (j + 1)) + "];", "and down: the even samples");
        out += field(type + " " + down + "_od[" + std::to_string(2 * w) + "];", "the odd samples");
        out += field("int " + down + "_pe;");
        out += field("int " + down + "_po;");
    }

    void emitHalfbandClear(std::string& out, int stage, Halfband kind)
    {
        const std::string up = pre(stage, 'u');
        const std::string down = pre(stage, 'd');
        if (kind == Halfband::Iir)
        {
            const std::string n = std::to_string(iirCoefficients(stage).size());
            out += "        for (int i = 0; i < " + n + "; ++i)\n        {\n";
            out += "            " + up + "_x[i] = 0;\n            " + up + "_y[i] = 0;\n";
            out += "            " + down + "_x[i] = 0;\n            " + down + "_y[i] = 0;\n        }\n";
            return;
        }
        const std::size_t j = firTaps(stage).size();
        const std::size_t w = 2 * j;
        out += "        " + up + "_pos = 0;\n        " + down + "_pe = 0;\n        " + down + "_po = 0;\n";
        out += "        for (int i = 0; i < " + std::to_string(2 * w) + "; ++i)\n        {\n";
        out += "            " + up + "_d[i] = 0;\n            " + down + "_od[i] = 0;\n        }\n";
        out += "        for (int i = 0; i < " + std::to_string(2 * (j + 1)) + "; ++i)\n";
        out += "            " + down + "_e[i] = 0;\n";
    }

    void emitHalfbandMethods(std::string& out, int stage, Halfband kind, const std::string& type)
    {
        const std::string s = std::to_string(stage);
        const std::string up = pre(stage, 'u');
        const std::string down = pre(stage, 'd');
        if (kind == Halfband::Iir)
        {
            const std::span<const double> a = iirCoefficients(stage);
            for (int direction = 0; direction < 2; ++direction)
            {
                const std::string& f = direction == 0 ? up : down;
                if (direction == 0)
                    out += "    void hb" + s + "_up(" + type + " x, " + type +
                           "* restrict o) [[clang::nonblocking]]   // " + rateText(stage) +
                           ", up: polyphase allpass\n    {\n        " + type + " s0 = x;\n        " + type +
                           " s1 = x;\n";
                else
                    out += "    " + type + " hb" + s + "_down(" + type + " v0, " + type +
                           " v1) [[clang::nonblocking]]   // and down\n    {\n        " + type + " s0 = v1;\n        " +
                           type + " s1 = v0;\n";
                for (std::size_t i = 0; i < a.size(); ++i)
                {
                    const std::string sv = i % 2 == 0 ? "s0" : "s1";
                    const std::string k = std::to_string(i);
                    out += "        {\n            const " + type + " t = " + sv + ";\n            " + sv + " = (" +
                           sv + " - " + f + "_y[" + k + "]) * " + literal(a[i]) + " + " + f + "_x[" + k +
                           "];\n            " + f + "_x[" + k + "] = t;\n            " + f + "_y[" + k + "] = " + sv +
                           ";\n        }\n";
                }
                if (direction == 0)
                    out += "        o[0] = s0;\n        o[1] = s1;\n    }\n";
                else
                    out += "        return 0.5f * (s0 + s1);\n    }\n";
            }
            return;
        }
        const std::span<const double> h2 = firTaps(stage);
        const std::size_t j = h2.size();
        const std::size_t w = 2 * j;
        const std::string sj = std::to_string(j);
        const std::string sw = std::to_string(w);
        out += "    void hb" + s + "_up(" + type + " x, " + type + "* restrict o) [[clang::nonblocking]]   // " +
               rateText(stage) + ", up: Kaiser FIR, " + sj + " host samples of delay\n    {\n";
        out += "        " + up + "_pos = " + up + "_pos == 0 ? " + std::to_string(w - 1) + " : " + up + "_pos - 1;\n";
        out += "        " + up + "_d[" + up + "_pos] = x;\n        " + up + "_d[" + up + "_pos + " + sw + "] = x;\n";
        out += "        const int p = " + up + "_pos;\n";
        out += "        o[0] = " + up + "_d[p + " + sj + "];\n        o[1] = ";
        for (std::size_t i = 0; i < j; ++i)
        {
            if (i > 0)
                out += "\n             + ";
            out += literal(h2[i]) + " * (" + up + "_d[p + " + std::to_string(j + i) + "] + " + up + "_d[p + " +
                   std::to_string(j - i - 1) + "])";
        }
        out += ";\n    }\n";
        out += "    " + type + " hb" + s + "_down(" + type + " v0, " + type +
               " v1) [[clang::nonblocking]]   // and down\n    {\n";
        out += "        " + down + "_pe = " + down + "_pe == 0 ? " + sj + " : " + down + "_pe - 1;\n";
        out += "        " + down + "_e[" + down + "_pe] = v0;\n        " + down + "_e[" + down + "_pe + " +
               std::to_string(j + 1) + "] = v0;\n";
        out += "        const int q = " + down + "_po;\n";
        out += "        const " + type + " y = 0.5f * " + down + "_e[" + down + "_pe + " + sj + "]";
        for (std::size_t i = 0; i < j; ++i)
            out += "\n                + " + literal(0.5 * h2[i]) + " * (" + down + "_od[q + " + std::to_string(j + i) +
                   "] + " + down + "_od[q + " + std::to_string(j - i - 1) + "])";
        out += ";\n";
        out +=
            "        " + down + "_po = " + down + "_po == 0 ? " + std::to_string(w - 1) + " : " + down + "_po - 1;\n";
        out += "        " + down + "_od[" + down + "_po] = v1;\n        " + down + "_od[" + down + "_po + " + sw +
               "] = v1;\n";
        out += "        return y;\n    }\n";
    }
} // namespace fminus::circuit
