// src/api/CircuitSelfTest.cpp: fminus::circuit::selfTest (include/fminus/Circuit.h), the part of the circuit
// compiler's self-test that needs the JIT, so it lives in fminus_lib rather than fminus_circuit. It plans the netlist,
// then runs the search of src/circuit/Search.h: the shared references first (the 64x chain for each unit T3 reads; for
// T1, the converged region at each rate, ADAA and integrator a candidate needs, on what the blocks before the region
// give it), then each candidate in order, generated with Options::chosen, compiled here through fminus::Program (on
// the calling thread, as the report's bench compiled them: Math::Vector, subnormals flushed, one thread, -O2) with the
// runners appended (src/circuit/Runners.h), and run on every unit through SelfTestOptions::parallel. Every number it
// judges by is computed in fminus_circuit; each job writes only its own unit's slot, and the slots are reduced in
// index order, so the answer and the notes are the same, byte for byte, for any number of threads.
#include "api/CircuitSelfTest.h"

#include "circuit/Analysis.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Runners.h"
#include "circuit/Search.h"
#include "fminus/FMinus.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace fminus::circuit
{
    namespace
    {
        // An instance of a module, aligned and zeroed, as a host makes one.
        class Instance
        {
        public:
            explicit Instance(const Program::Module& module)
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

            ~Instance()
            {
#if defined(_WIN32)
                _aligned_free(memory_);
#else
                std::free(memory_);
#endif
            }

            Instance(const Instance&) = delete;
            Instance& operator=(const Instance&) = delete;

            void* get() const
            {
                return memory_;
            }

        private:
            void* memory_ = nullptr;
        };

        // A candidate's module, compiled with its runners.
        struct Compiled
        {
            std::unique_ptr<Program> program;
            const Program::Module* module = nullptr;
            void (*init)(void*, float) = nullptr;
            void (*set)(void*, const float*, const int*) = nullptr;
            void (*steps)(void*, const float*, const float*, float*, int) = nullptr;
            void (*ticks)(void*, const float*, const float*, const float*, const int*, float*, int, int) = nullptr;
        };

        Compiled compile(const Result& result, const std::string& fileName, bool region)
        {
            RunnerShape shape;
            shape.module = result.module;
            shape.pots = static_cast<int>(result.pots.size());
            shape.switches = static_cast<int>(result.switches.size());
            shape.laws = static_cast<int>(result.inputs.size()) - 1;
            shape.channels = 1;
            shape.step = region;
            Program::Options o;
            o.fileName = fileName + ".fm";
            o.math = Program::Math::Vector;
            o.denormals = Program::Denormals::Flush;
            o.threads = 1;
            o.optimisation = 2;
            std::string errors;
            Compiled c;
            c.program = Program::compile(result.source + runnerSource(shape), errors, o);
            if (!c.program)
                throw std::runtime_error("a candidate's module does not compile:\n" + errors);
            c.module = c.program->module(result.module);
            c.init = c.program->function<void(void*, float)>(result.module + "::init", errors);
            c.set = c.program->function<void(void*, const float*, const int*)>("fm_set", errors);
            if (region)
                c.steps = c.program->function<void(void*, const float*, const float*, float*, int)>("fm_steps", errors);
            c.ticks = c.program->function<void(void*, const float*, const float*, const float*, const int*, float*, int,
                                               int)>("fm_ticks", errors);
            if (c.module == nullptr || c.init == nullptr || c.set == nullptr || c.ticks == nullptr ||
                (region && c.steps == nullptr))
                throw std::runtime_error("a candidate's module lacks a method or a runner: " + errors);
            return c;
        }

        std::vector<float> floats(const std::vector<double>& v)
        {
            return {v.begin(), v.end()};
        }

        std::vector<double> doubles(const std::vector<float>& v)
        {
            return {v.begin(), v.end()};
        }

        std::string openText(const Config& c)
        {
            std::string text;
            auto add = [&](bool open, const char* name)
            {
                if (open)
                    text += (text.empty() ? "" : ", ") + std::string(name);
            };
            add(c.oversampleOpen, ".oversample");
            add(c.adaaOpen, ".adaa");
            add(c.solverOpen, ".solver");
            add(c.methodOpen, ".method");
            return text;
        }
    } // namespace

    SelfTestResult selfTestWith(std::string_view netlist, std::string_view fileName, const Options& options,
                                const SelfTestOptions& test, const StressShape& shape)
    {
        SelfTestResult out;
        const std::string file(fileName);
        Messages notes;
        auto finish = [&]
        {
            out.notes = notes.format(file, Severity::Note);
            return out;
        };
        try
        {
            // the netlist's own messages, as generate gives them; the configuration without any answer, and the
            // candidates measured as a host plays them without probes (Options::probes changes no output bit, and is
            // not part of the key)
            Options base = options;
            base.chosen.reset();
            base.channels.reset();
            base.probes = false;
            const Result first = generate(netlist, fileName, base);
            out.errors = first.errors;
            out.warnings = first.warnings;
            if (!first.errors.empty())
                return finish();
            Messages scratch;
            const Netlist parsed = parse(netlist, scratch);
            const std::optional<Plan> planned = plan(parsed, base, fileName, scratch);
            if (!planned)
                throw std::logic_error("the plan failed where generate did not");
            const Plan& p = *planned;
            const Config& cfg = p.config;
            out.key = p.key;
            if (!cfg.anyOpen())
            {
                notes.note({}, "self-test " + p.key +
                                   ": the netlist names its whole configuration (or has no port): nothing to choose");
                return finish();
            }
            const StressMatrix matrix = stressMatrix(p, shape);
            std::vector<Candidate> order = candidates(p);
            const int units = matrix.units();
            // the method check (stage 7): the oversampling factors at which a region block's linearised trapezoidal
            // A has an eigenvalue near -1, which the references below measure
            std::vector<int> factors;
            for (const Candidate& c : order)
            {
                if (c.method == Method::Tr && std::find(factors.begin(), factors.end(), c.oversample) == factors.end())
                    factors.push_back(c.oversample);
            }
            std::sort(factors.begin(), factors.end());
            std::vector<Ringing> ringing = ringingFlags(p, factors);
            const bool flagged =
                std::any_of(ringing.begin(), ringing.end(), [](const Ringing& r) { return r.block >= 0; });
            notes.note({}, std::format("self-test {}: {} open; {} point{} x {} signals = {} units, L = {:g} V ({}); {} "
                                       "candidates, cheapest first",
                                       p.key, openText(cfg), matrix.points.size(), matrix.points.size() == 1 ? "" : "s",
                                       matrix.signals.size(), units, matrix.level, matrix.levelFrom, order.size()));
            const std::int64_t budget = test.budget > 0 ? test.budget : kDefaultBudget;
            std::int64_t used = 0;
            const Stop stop = test.cancelled;
            auto cancelled = [&]
            {
                return test.cancelled && test.cancelled();
            };
            // A job's exception (std::bad_alloc on a unit's buffers, say) is caught on the job's own thread, so a
            // host's pool needs no try/catch, and rethrown here, the lowest index's, so the error is the same for
            // any number of threads.
            auto run = [&](int count, const std::function<void(int)>& job)
            {
                std::vector<std::exception_ptr> failed(static_cast<std::size_t>(count));
                const std::function<void(int)> guarded = [&](int i)
                {
                    try
                    {
                        job(i);
                    }
                    catch (...)
                    {
                        failed[static_cast<std::size_t>(i)] = std::current_exception();
                    }
                };
                if (test.parallel)
                    test.parallel(count, guarded);
                else
                {
                    for (int i = 0; i < count; ++i)
                        guarded(i);
                }
                for (const std::exception_ptr& e : failed)
                {
                    if (e)
                        std::rethrow_exception(e);
                }
            };
            auto afford = [&](std::int64_t need, const std::string& what)
            {
                if (used + need <= budget)
                {
                    used += need;
                    return true;
                }
                notes.note({}, std::format("the self-test stops before {}: it needs {} steps more, and {} of its "
                                           "budget of {} are spent; no answer",
                                           what, need, used, budget));
                return false;
            };
            auto stopped = [&]
            {
                out.cancelled = true;
                notes.note({}, "the self-test was cancelled; no answer");
                return finish();
            };

            // the 64x references, once a unit
            if (cancelled())
                return stopped();
            if (!afford(steps64(matrix), std::format("the {}x references", shape.referenceFactor)))
                return finish();
            std::vector<Reference64> ref64(static_cast<std::size_t>(units));
            run(units,
                [&](int u)
                {
                    if (!matrix.signal(u).t2 || cancelled())
                        return;
                    if (!matrix.runs(u))
                        return;
                    ref64[static_cast<std::size_t>(u)] = reference64(p, matrix, u, stop, flagged);
                });
            if (cancelled())
                return stopped();
            long unconverged64 = 0;
            for (const Reference64& r : ref64)
                unconverged64 += r.unconverged;

            // the candidates, each with its configuration's T1 references (shared by its Newton steps)
            std::map<std::tuple<int, bool, Method>, std::vector<ReferenceT1>> t1;
            bool overBudget = false;
            // a flagged factor's trapezoidal reference (no ADAA), with its energy above 20 kHz: against the 64x
            // reference's in the same band, summed over the units both run
            for (Ringing& r : ringing)
            {
                if (r.block < 0)
                    continue;
                const Block& block = p.blocks[static_cast<std::size_t>(r.block)];
                notes.note({}, std::format("self-test: at {}x block {}'s linearised trapezoidal A has an eigenvalue at "
                                           "{:.4f} (within 0.1 of -1): it may ring at Nyquist",
                                           r.oversample, block.index, r.eigenvalue.real()));
                if (cancelled())
                    return stopped();
                if (!afford(stepsT1(matrix, r.oversample), std::format("the T1 references at {}x", r.oversample)))
                    return finish();
                std::vector<ReferenceT1> refs(static_cast<std::size_t>(units));
                run(units,
                    [&](int u)
                    {
                        if (!matrix.signal(u).t1 || !matrix.runs(u) || cancelled())
                            return;
                        refs[static_cast<std::size_t>(u)] =
                            referenceT1(p, matrix, u, r.oversample, false, Method::Tr, stop, true);
                    });
                if (cancelled())
                    return stopped();
                double here = 0;
                double truth = 0;
                for (int u = 0; u < units; ++u)
                {
                    const ReferenceT1& a = refs[static_cast<std::size_t>(u)];
                    const Reference64& b = ref64[static_cast<std::size_t>(u)];
                    if (!a.done || !b.done || a.high < 0)
                        continue;
                    here += a.high;
                    truth += b.high;
                }
                r.measured = true;
                r.excess = 10 * std::log10(std::max(here, 1e-300) / std::max(truth, 1e-300));
                notes.note({}, std::format("self-test: at {}x block {} puts {:.1f} dB more above 20 kHz than the 64x "
                                           "reference{}",
                                           r.oversample, block.index, r.excess,
                                           r.rings() ? ": it rings" : ": no ringing (6 dB is the gate)"));
                t1.emplace(std::make_tuple(r.oversample, false, Method::Tr), std::move(refs));
            }
            // each factor that rings, a warning (once every flagged factor is measured: it suggests the next that
            // does not)
            for (const Ringing& r : ringing)
            {
                if (!r.rings())
                    continue;
                const Block& block = p.blocks[static_cast<std::size_t>(r.block)];
                const Loc loc = p.netlist.elements[static_cast<std::size_t>(block.elements.front())].loc;
                out.warnings +=
                    std::format("{}:{}:{}: warning: {}\n", file, loc.line, loc.column, ringingWarning(p, r, ringing));
            }
            // a ringing factor's candidates each get a BDF2 twin right after them (no ADAA: it is the trapezoidal
            // rule's), where the integrator is open and ADAA not named on
            if (cfg.methodOpen && !(cfg.adaa && !cfg.adaaOpen))
            {
                const std::size_t before = order.size();
                order = withTwins(p, order, ringing);
                if (order.size() > before)
                    notes.note({}, std::format("self-test: {} BDF2 twin{} join the candidates, each right after its "
                                               "trapezoidal candidate ({} candidates)",
                                               order.size() - before, order.size() - before == 1 ? "" : "s",
                                               order.size()));
            }
            int index = 0;
            const auto measure = [&](const Candidate& c) -> std::optional<Verdict>
            {
                ++index;
                const auto key = std::make_tuple(c.oversample, c.adaa, c.method);
                if (!t1.count(key))
                {
                    if (cancelled())
                        return std::nullopt;
                    if (!afford(stepsT1(matrix, c.oversample),
                                std::format("the T1 references at {}x{}", c.oversample, c.adaa ? " with ADAA" : "")))
                    {
                        overBudget = true;
                        return std::nullopt;
                    }
                    std::vector<ReferenceT1> refs(static_cast<std::size_t>(units));
                    run(units,
                        [&](int u)
                        {
                            if (!matrix.signal(u).t1 || !matrix.runs(u) || cancelled())
                                return;
                            refs[static_cast<std::size_t>(u)] =
                                referenceT1(p, matrix, u, c.oversample, c.adaa, c.method, stop);
                        });
                    if (cancelled())
                        return std::nullopt;
                    t1.emplace(key, std::move(refs));
                }
                const std::vector<ReferenceT1>& refs = t1.at(key);
                if (cancelled())
                    return std::nullopt;
                const std::string name = "candidate " + std::to_string(index) + ", " + c.text();
                if (!afford(stepsCandidate(matrix, c.oversample), name))
                {
                    overBudget = true;
                    return std::nullopt;
                }
                Options o = base;
                o.chosen = choiceOf(p.key, c, Verdict{}, true);
                o.channels = 1;
                const Result generated = generate(netlist, fileName, o);
                if (!generated.errors.empty())
                    throw std::logic_error("a candidate does not generate:\n" + generated.errors);
                const Compiled compiled = compile(generated, file, p.regionFirst >= 0);
                const std::size_t host = matrix.shape.host();
                const auto pots = static_cast<std::size_t>(p.pots.size());
                const auto switches = static_cast<std::size_t>(p.switches.size());
                std::vector<UnitVerdict> results(static_cast<std::size_t>(units));
                run(units,
                    [&](int u)
                    {
                        if (cancelled() || !matrix.runs(u))
                            return;
                        const StressPoint& point = matrix.point(u);
                        const StressSignal& signal = matrix.signal(u);
                        // the point's pots, then its switches' positions (set()'s ints)
                        const std::vector<float> potValues(point.pots.begin(),
                                                           point.pots.begin() + static_cast<long>(pots));
                        std::vector<int> switchValues;
                        for (std::size_t k = 0; k < switches; ++k)
                            switchValues.push_back(static_cast<int>(point.pots[pots + k]));
                        // set(), until a switch's ramp has settled (60 calls at 48 kHz; once without switches)
                        auto settle = [&](void* self)
                        {
                            for (int k = 0; k < (switches > 0 ? 64 : 1); ++k)
                                compiled.set(self, pots > 0 ? potValues.data() : nullptr,
                                             switches > 0 ? switchValues.data() : nullptr);
                        };
                        const bool laws = !p.laws.empty();
                        // the region's step at its rate, on what the region sees (T1's reference's input), the law
                        // inputs held over each host sample's steps
                        std::vector<float> stepsOut;
                        if (compiled.steps != nullptr)
                        {
                            const std::size_t n = static_cast<std::size_t>(c.oversample) * host;
                            const std::vector<float> in = floats(regionInput(p, matrix, u, c.oversample, stop));
                            const std::vector<float> law =
                                laws ? floats(lawArguments(p, matrix, u, c.oversample, n)) : std::vector<float>{};
                            stepsOut.resize(n);
                            Instance self(*compiled.module);
                            compiled.init(self.get(), static_cast<float>(kHostRate));
                            settle(self.get());
                            compiled.steps(self.get(), in.data(), laws ? law.data() : nullptr, stepsOut.data(),
                                           static_cast<int>(n));
                        }
                        // tick at the host's, set() on the grid
                        const std::vector<float> in = floats(signalSamples(matrix, signal, 1, host));
                        std::vector<float> potBuffer(host * pots);
                        std::vector<int> switchBuffer(host * switches);
                        for (std::size_t i = 0; i < host; ++i)
                        {
                            std::copy(potValues.begin(), potValues.end(),
                                      potBuffer.begin() + static_cast<long>(i * pots));
                            std::copy(switchValues.begin(), switchValues.end(),
                                      switchBuffer.begin() + static_cast<long>(i * switches));
                        }
                        std::vector<float> ticksOut(host);
                        const std::vector<float> law =
                            laws ? floats(lawArguments(p, matrix, u, 1, host)) : std::vector<float>{};
                        Instance self(*compiled.module);
                        compiled.init(self.get(), static_cast<float>(kHostRate));
                        if (switches > 0)
                            settle(self.get());
                        compiled.ticks(self.get(), in.data(), laws ? law.data() : nullptr,
                                       pots > 0 ? potBuffer.data() : nullptr,
                                       switches > 0 ? switchBuffer.data() : nullptr, ticksOut.data(),
                                       static_cast<int>(host), 0);
                        const auto unit = static_cast<std::size_t>(u);
                        // tick adds the output's DC operating point back; the references run in the offsets
                        std::vector<double> ticked = doubles(ticksOut);
                        if (p.outputDc != 0)
                        {
                            for (double& y : ticked)
                                y -= p.outputDc;
                        }
                        results[unit] = measureUnit(matrix, u, c.oversample, doubles(stepsOut), ticked,
                                                    signal.t1 ? &refs[unit] : nullptr,
                                                    signal.t2 ? &ref64[unit] : nullptr);
                    });
                if (cancelled())
                    return std::nullopt;
                long unconverged = unconverged64;
                for (const ReferenceT1& r : refs)
                    unconverged += r.unconverged;
                Verdict v = reduce(matrix, results, unconverged);
                notes.note({}, std::format("self-test candidate {}: {}, {:.0f} model cycles: {}: {}", index, c.text(),
                                           c.cost, v.figures(),
                                           v.passes() ? std::string("passes") : "fails (" + v.worst() + ")"));
                return v;
            };
            const SearchOutcome found = search(order, measure, cancelled, test.everyCandidate);
            out.candidates = static_cast<int>(found.tried.size());
            if (found.stopped)
            {
                if (!overBudget)
                    return stopped();
                return finish();
            }
            if (found.chosen < 0)
            {
                notes.note({}, std::format("self-test: no candidate under {:g} model cycles is finite on every signal "
                                           "with converged references; no answer",
                                           kCostCap));
                return finish();
            }
            const Tried& t = found.tried[static_cast<std::size_t>(found.chosen)];
            out.choice = choiceOf(p.key, t.candidate, t.verdict, found.met);
            if (found.met)
                notes.note({}, std::format("self-test: the answer is candidate {}, {}: the first that meets T1-T4",
                                           found.chosen + 1, t.candidate.text()));
            else
                notes.note({}, std::format("self-test: no candidate meets T1-T4; the answer is the most accurate under "
                                           "{:g} model cycles, candidate {}, {} (worst: {})",
                                           kCostCap, found.chosen + 1, t.candidate.text(), t.verdict.worst()));
            notes.note({}, "the self-test chose " + directivesOf(cfg, *out.choice));
            return finish();
        }
        catch (const std::exception& e)
        {
            SelfTestResult failed;
            failed.key = out.key;
            failed.errors = file + ":1:1: error: internal error: " + e.what() + "\n";
            return failed;
        }
        catch (...)
        {
            SelfTestResult failed;
            failed.key = out.key;
            failed.errors = file + ":1:1: error: internal error: an exception that is not a std::exception\n";
            return failed;
        }
    }

    SelfTestResult selfTest(std::string_view netlist, std::string_view fileName, const Options& options,
                            const SelfTestOptions& test)
    {
        return selfTestWith(netlist, fileName, options, test, StressShape{});
    }

    SelfTestResult selfTest(std::string_view netlist, std::string_view fileName, const Options& options)
    {
        return selfTestWith(netlist, fileName, options, SelfTestOptions{}, StressShape{});
    }
} // namespace fminus::circuit
