// src/circuit/Generate.cpp: fminus::circuit::generate (include/fminus/Circuit.h): parse, plan, each block's
// equations and pivots, then the module's text. Any error stops before emission.
#include "circuit/Emit.h"
#include "circuit/Halfbands.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "fminus/Circuit.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace fminus::circuit
{
    namespace
    {
        // Options out of range, as "file:1:1: error: ..." lines.
        std::string checkOptions(const Options& o, const std::string& file)
        {
            std::string errors;
            auto fail = [&](const std::string& text)
            {
                errors += file + ":1:1: error: " + text + "\n";
            };
            if (o.oversample && *o.oversample != 1 && *o.oversample != 2 && *o.oversample != 4 && *o.oversample != 8)
                fail("Options::oversample is " + std::to_string(*o.oversample) + "; it is 1, 2, 4 or 8");
            if (o.channels && *o.channels != 1 && *o.channels != 2)
                fail("Options::channels is " + std::to_string(*o.channels) + "; it is 1 or 2");
            if (o.solver)
            {
                const int lo = o.solver->guess == Solver::Guess::Omega ? 0 : 1;
                if (o.solver->steps < lo || o.solver->steps > 8)
                    fail("Options::solver's steps are " + std::to_string(o.solver->steps) + "; they are " +
                         std::to_string(lo) + " to 8");
            }
            if (!o.moduleName.empty() && !isPlainName(o.moduleName))
                fail("Options::moduleName '" + o.moduleName + "' is not a letter followed by letters and digits");
            if (o.chosen)
            {
                const Choice& c = *o.chosen;
                const bool hex =
                    c.key.size() == 16 && std::all_of(c.key.begin(), c.key.end(), [](char k)
                                                      { return (k >= '0' && k <= '9') || (k >= 'a' && k <= 'f'); });
                if (!hex)
                    fail("Options::chosen's key '" + c.key + "' is not 16 hex digits");
                if (c.oversample != 1 && c.oversample != 2 && c.oversample != 4 && c.oversample != 8)
                    fail("Options::chosen's oversample is " + std::to_string(c.oversample) + "; it is 1, 2, 4 or 8");
                const int lo = c.solver.guess == Solver::Guess::Omega ? 0 : 1;
                if (c.solver.steps < lo || c.solver.steps > 8)
                    fail("Options::chosen's solver steps are " + std::to_string(c.solver.steps) + "; they are " +
                         std::to_string(lo) + " to 8");
            }
            return errors;
        }

        std::string partsText(const Plan& plan, const Block& block)
        {
            std::string out;
            for (int e : block.elements)
                out += (out.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
            return out;
        }

        // The answer the self-test gave when nothing met the targets (Choice::met false): a warning at the region's
        // first port, naming the worst gate and the configuration used.
        void noConfiguration(const Plan& plan, Messages& messages)
        {
            const Config& c = plan.config;
            for (int i = plan.regionFirst; i <= plan.regionLast; ++i)
            {
                const Block& b = plan.blocks[static_cast<std::size_t>(i)];
                if (!b.port)
                    continue;
                std::string parts;
                for (int e : b.port->elements)
                    parts += (parts.empty() ? "" : ", ") + plan.netlist.elements[static_cast<std::size_t>(e)].name;
                messages.warning(plan.netlist.elements[static_cast<std::size_t>(b.port->elements.front())].loc,
                                 "block " + std::to_string(b.index) + " (" + parts +
                                     "): no configuration meets the targets (worst: " + c.chosen->worst +
                                     "); using the most accurate under the cost cap: " +
                                     configurationText(c.oversample, c.halfband, c.adaa, c.solver, false));
                return;
            }
        }

        // The plan as notes: each block, its rate and solver (and the form of its b<N>_set), and the latency.
        void planNotes(const Plan& plan, const std::vector<BlockCode>& codes, Messages& messages)
        {
            const Config& c = plan.config;
            for (const Block& b : plan.blocks)
            {
                int states = 0;
                for (const Branch& br : b.branches)
                    states += br.state() ? 1 : 0;
                std::string text = "block " + std::to_string(b.index) + " (" + partsText(plan, b) +
                                   "): " + std::to_string(states) + (states == 1 ? " state, " : " states, ");
                const std::string rate =
                    b.rateFactor == 1 ? std::string("the host rate") : std::to_string(b.rateFactor) + " rate";
                if (b.port)
                {
                    text += "port " + b.port->parts + ", at " + rate;
                    if (c.adaa)
                        text += " (designed at " + rate + " 2/3: ADAA)";
                    text += std::string(", ") + (c.solver.guess == Solver::Guess::Omega ? "omega3" : "the previous v") +
                            " + " + std::to_string(c.solver.steps) +
                            (c.solver.steps == 1 ? " Newton step" : " Newton steps");
                }
                else
                {
                    text += "linear, at " + rate + std::format(" (1x check: {:.2f} dB off at 44.1 kHz", b.oneX);
                    if (b.inRegion)
                        text += std::format(", {:.2f} dB at {}x", b.oneXRegion, b.rateFactor);
                    if (b.design == Block::Design::Matched)
                        text += std::format("; a matched design, {:.2f} dB at 44.1 and 48 kHz", b.matchedError);
                    text += ")";
                }
                messages.note({}, text);
                const std::string& form = codes[static_cast<std::size_t>(b.index - 1)].form;
                if (!form.empty())
                    messages.note({}, "block " + std::to_string(b.index) + "'s " + form);
            }
            if (plan.regionFirst >= 0 && c.oversample > 1)
            {
                std::string text = std::format("latency: about {:.1f} host samples at 1 kHz ({} halfbands)",
                                               roundTripDelay(c.oversample, c.halfband, 1000),
                                               c.halfband == Halfband::Iir ? "IIR" : "FIR");
                if (const double adaa = adaaLatency(plan); adaa > 0)
                    text += std::format(", plus {:g} (ADAA)", adaa);
                messages.note({}, text);
            }
        }
    } // namespace

    Result generate(std::string_view netlist, std::string_view fileName, const Options& options)
    {
        Result result;
        const std::string file(fileName);
        try
        {
            if (std::string errors = checkOptions(options, file); !errors.empty())
            {
                result.errors = errors;
                return result;
            }
            Messages messages;
            const Netlist parsed = parse(netlist, messages);
            std::optional<Plan> p;
            if (!messages.hasErrors())
                p = plan(parsed, options, fileName, messages);
            std::vector<BlockCode> blocks;
            std::vector<ProbeNode> nodes;
            if (p && !messages.hasErrors())
            {
                nodes = probeNodes(*p, parsed);
                for (std::size_t b = 0; b < p->blocks.size(); ++b)
                {
                    // with probes, the readable nodes the block computes are readouts of its derivation too
                    std::vector<int> probes;
                    for (const ProbeNode& n : nodes)
                    {
                        if (options.probes && n.node.readable && n.block == static_cast<int>(b))
                            probes.push_back(n.index);
                    }
                    std::optional<BlockCode> code = deriveBlock(*p, static_cast<int>(b), messages, true, probes);
                    if (!code)
                        break;
                    blocks.push_back(std::move(*code));
                }
            }
            if (p && !messages.hasErrors())
            {
                result.source = emit(*p, blocks, &result.lines, options.probes ? &nodes : nullptr);
                for (const ProbeNode& n : nodes)
                    result.nodes.push_back(n.node);
                result.module = p->module;
                for (const PotInfo& pot : p->pots)
                    result.pots.push_back(pot.name);
                for (const SwitchInfo& w : p->switches)
                    result.switches.push_back(w.name);
                Input signal;
                signal.name = p->inputName;
                signal.scale = p->inputScale;
                result.inputs.push_back(signal);
                for (const LawInfo& l : p->laws)
                    result.inputs.push_back({l.name, l.kind, l.scale, l.low, l.high});
                const Config& c = p->config;
                result.configuration = configText(c);
                result.provisional = c.provisional;
                result.selfTestKey = c.anyOpen() ? p->key : std::string();
                if (p->regionFirst >= 0)
                    result.latency =
                        (c.oversample > 1 ? roundTripDelay(c.oversample, c.halfband, 1000) : 0) + adaaLatency(*p);
                if (c.chosen && !c.chosen->met)
                    noConfiguration(*p, messages);
                planNotes(*p, blocks, messages);
            }
            result.errors = messages.format(file, Severity::Error);
            result.warnings = messages.format(file, Severity::Warning);
            result.notes = messages.format(file, Severity::Note);
            if (!result.errors.empty())
            {
                const std::string errors = result.errors;
                const std::string warnings = result.warnings;
                const std::string notes = result.notes;
                result = Result{};
                result.errors = errors;
                result.warnings = warnings;
                result.notes = notes;
            }
        }
        catch (const NotAFloat& e)
        {
            result = Result{};
            result.errors = file + ": error: " + e.what() + "\n";
        }
        catch (const std::exception& e)
        {
            result = Result{};
            result.errors = file + ":1:1: error: internal error: " + e.what() + "\n";
        }
        return result;
    }

    Result generate(std::string_view netlist, std::string_view fileName)
    {
        return generate(netlist, fileName, Options{});
    }

    std::string selfTestKey(std::string_view netlist, std::string_view fileName, const Options& options)
    {
        try
        {
            Messages messages;
            const Netlist parsed = parse(netlist, messages);
            if (messages.hasErrors())
                return {};
            Options o = options;
            o.chosen.reset();
            const std::optional<Plan> p = plan(parsed, o, fileName, messages);
            return p && !messages.hasErrors() ? p->key : std::string();
        }
        catch (const std::exception&)
        {
            return {};
        }
    }
} // namespace fminus::circuit
