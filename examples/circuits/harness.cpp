// examples/circuits/harness.cpp: the shared part of example-circuits (harness.h).
#include "harness.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace circuits
{
    std::string netlist(const std::string& name)
    {
        std::ifstream file(std::string(FMINUS_EXAMPLE_DIR) + "/" + name);
        if (!file)
            throw std::runtime_error("cannot read " + name);
        std::stringstream s;
        s << file.rdbuf();
        return s.str();
    }

    std::string withDirective(std::string text, const std::string& directive, const std::string& line)
    {
        std::size_t at = 0;
        while (at < text.size())
        {
            const std::size_t end = text.find('\n', at);
            const std::string current = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (current.rfind(directive, 0) == 0)
            {
                text.replace(at, current.size(), line);
                return text;
            }
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        const std::size_t endLine = text.find("\n.end");
        if (endLine == std::string::npos)
            return text + line + "\n";
        return text.insert(endLine + 1, line + "\n");
    }

    std::string withN(std::string text, const std::string& n)
    {
        const std::size_t at = text.find("N=");
        if (at == std::string::npos)
            throw std::runtime_error("no N= in the netlist's .model");
        const std::size_t end = text.find_first_of(" )\n", at);
        return text.replace(at, end - at, "N=" + n);
    }

    Instance::Instance(const fminus::Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
        memory_ = _aligned_malloc(size, alignment);
#else
        memory_ = std::aligned_alloc(alignment, size);
#endif
        if (memory_ == nullptr)
            throw std::bad_alloc();
        std::memset(memory_, 0, module.size);
    }

    Instance::~Instance()
    {
#if defined(_WIN32)
        _aligned_free(memory_);
#else
        std::free(memory_);
#endif
    }

    void Circuit::callSet(void* self, std::span<const double> pots) const
    {
        const auto p = [&](std::size_t i)
        {
            return static_cast<float>(pots[i]);
        };
        switch (pots.size())
        {
        case 0:
            return;
        case 1:
            reinterpret_cast<void (*)(void*, float)>(set)(self, p(0));
            return;
        case 2:
            reinterpret_cast<void (*)(void*, float, float)>(set)(self, p(0), p(1));
            return;
        case 3:
            reinterpret_cast<void (*)(void*, float, float, float)>(set)(self, p(0), p(1), p(2));
            return;
        default:
            throw std::runtime_error("set() with more than 3 pots");
        }
    }

    std::unique_ptr<Circuit> build(const std::string& text, const std::string& fileName, const fc::Options& options,
                                   std::string& errors, fminus::Program::Math math, const std::string& extra)
    {
        auto c = std::make_unique<Circuit>();
        c->fileName = fileName;
        c->result = fc::generate(text, fileName, options);
        errors = c->result.errors + c->result.warnings;
        if (!c->result.errors.empty())
            return nullptr;
        fc::Messages messages;
        const fc::Netlist parsed = fc::parse(text, messages);
        std::optional<fc::Plan> plan = fc::plan(parsed, options, fileName, messages);
        if (!plan)
        {
            errors += messages.format(fileName, fc::Severity::Error);
            return nullptr;
        }
        c->plan = *plan;
        const int channels = c->plan.config.channels;
        const bool region = c->plan.regionFirst >= 0;
        fminus::Program::Options o;
        o.fileName = fileName + ".fm";
        o.math = math;
        o.denormals = fminus::Program::Denormals::Flush;
        o.threads = 1;
        std::string fmErrors;
        fc::RunnerShape shape;
        shape.module = c->result.module;
        shape.pots = c->pots();
        shape.switches = c->switches();
        shape.laws = c->laws();
        shape.channels = channels;
        shape.step = region;
        c->program = fminus::Program::compile(c->result.source + fc::runnerSource(shape) + extra, fmErrors, o);
        if (!c->program)
        {
            errors += fmErrors;
            return nullptr;
        }
        const std::string& m = c->result.module;
        c->module = c->program->module(m);
        c->init = c->program->function<void(void*, float)>(m + "::init", fmErrors);
        c->clear = c->program->function<void(void*)>(m + "::clear", fmErrors);
        if (c->pots() > 0)
            c->set = c->program->address(m + "::set", fmErrors);
        c->setAll = c->program->function<void(void*, const float*, const int*)>("fm_set", fmErrors);
        if (region && channels == 1)
            c->steps = c->program->function<void(void*, const float*, const float*, float*, int)>("fm_steps", fmErrors);
        if (channels == 1)
            c->ticks = c->program->function<void(void*, const float*, const float*, const float*, const int*, float*,
                                                 int, int)>("fm_ticks", fmErrors);
        else
            c->ticks2 = c->program->function<void(void*, const float*, const float*, const float*, const float*,
                                                  const int*, float*, float*, int, int)>("fm_ticks2", fmErrors);
        errors += fmErrors;
        return c;
    }

    namespace
    {
        // The law buffer a run reads: `law`, or each law input at its low end (as an argument: over its scale).
        std::vector<float> lawsOf(const Circuit& c, std::span<const float> law, std::size_t n)
        {
            if (!law.empty() || c.laws() == 0)
                return {law.begin(), law.end()};
            std::vector<double> lows;
            for (const fc::LawInfo& l : c.plan.laws)
                lows.push_back(l.low / l.scale);
            return heldLaws(lows, n);
        }
    } // namespace

    std::vector<float> heldLaws(std::span<const double> values, std::size_t n)
    {
        std::vector<float> out;
        out.reserve(n * values.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            for (double v : values)
                out.push_back(static_cast<float>(v));
        }
        return out;
    }

    std::vector<double> runSteps(const Circuit& c, std::span<const double> u, std::span<const double> pots,
                                 std::span<const float> law)
    {
        Instance self(*c.module);
        c.init(self.get(), 48000);
        c.callSet(self.get(), pots);
        std::vector<float> in(u.begin(), u.end());
        std::vector<float> out(u.size());
        const std::vector<float> laws = lawsOf(c, law, u.size());
        c.steps(self.get(), in.data(), laws.empty() ? nullptr : laws.data(), out.data(), static_cast<int>(in.size()));
        return {out.begin(), out.end()};
    }

    std::vector<double> runTicksWith(const Circuit& c, std::span<const double> u, std::span<const float> potBuffer,
                                     int call, std::span<const float> law, std::span<const int> switchBuffer)
    {
        Instance self(*c.module);
        c.init(self.get(), 48000);
        const auto n = static_cast<int>(u.size());
        std::vector<float> in(u.begin(), u.end());
        std::vector<float> out(u.size());
        const int size = call <= 0 ? n : call;
        const auto p = static_cast<std::size_t>(c.pots());
        const auto l = static_cast<std::size_t>(c.laws());
        const std::vector<float> laws = lawsOf(c, law, u.size());
        for (int start = 0; start < n; start += size)
        {
            const int count = std::min(size, n - start);
            const auto s = static_cast<std::size_t>(start);
            const auto w = static_cast<std::size_t>(c.switches());
            c.ticks(self.get(), in.data() + s, laws.empty() ? nullptr : laws.data() + s * l,
                    potBuffer.empty() ? nullptr : potBuffer.data() + s * p,
                    switchBuffer.empty() ? nullptr : switchBuffer.data() + s * w, out.data() + s, count, start);
        }
        return {out.begin(), out.end()};
    }

    std::vector<double> runTicks(const Circuit& c, std::span<const double> u, std::span<const double> pots, int call,
                                 std::span<const float> law)
    {
        std::vector<float> buffer;
        buffer.reserve(u.size() * pots.size());
        for (std::size_t i = 0; i < u.size(); ++i)
        {
            for (double v : pots)
                buffer.push_back(static_cast<float>(v));
        }
        return runTicksWith(c, u, buffer, call, law);
    }

    std::vector<double> potsWith(const fc::Plan& plan, const std::string& key, double value)
    {
        std::vector<double> point;
        for (const fc::PotInfo& p : plan.pots)
            point.push_back(p.key == key ? value : p.defaultValue);
        return point;
    }

    std::vector<fc::Holters> chainAt(const fc::Plan& plan, std::span<const double> pots, double rate, bool adaa,
                                     std::span<const double> laws)
    {
        std::vector<fc::Holters> blocks;
        const std::vector<double> point(pots.begin(), pots.end());
        const std::vector<double> lawPoint(laws.begin(), laws.end());
        for (std::size_t b = 0; b < plan.blocks.size(); ++b)
        {
            const double design = plan.blocks[b].port && adaa ? rate * 2 / 3 : rate;
            blocks.push_back(fc::holters(plan, static_cast<int>(b), design, point, lawPoint));
        }
        return blocks;
    }

    double dB(double ratio)
    {
        return 20 * std::log10(ratio);
    }

    bool finite(std::span<const double> y)
    {
        return std::all_of(y.begin(), y.end(), [](double v) { return std::isfinite(v); });
    }
} // namespace circuits
