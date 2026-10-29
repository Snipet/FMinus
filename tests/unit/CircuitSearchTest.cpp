// tests/unit/CircuitSearchTest.cpp: the circuit compiler's self-test (src/circuit/Search.h,
// src/api/CircuitSelfTest.cpp): the candidates' cost-model order, the ordered search and the no-configuration choice on
// stub measures, the key, a Choice's text and what generate makes of one, and the self-test itself, run through the JIT
// on a small stress shape: T1 on the region's own input, its answer the same for any number of threads, its cancel and
// its budget.
#include "api/CircuitSelfTest.h"
#include "circuit/Netlist.h"
#include "circuit/Plan.h"
#include "circuit/Search.h"
#include "CircuitNetlists.h"
#include "fminus/Circuit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fc = fminus::circuit;

    fc::Plan planned(const std::string& text, const fc::Options& options = {})
    {
        fc::Messages messages;
        const fc::Netlist netlist = fc::parse(text, messages);
        std::optional<fc::Plan> p = fc::plan(netlist, options, "t.cir", messages);
        INFO(messages.format("t.cir", fc::Severity::Error));
        REQUIRE(p);
        return *p;
    }

    std::string replaced(std::string text, const std::string& from, const std::string& to)
    {
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        return text.replace(at, from.size(), to);
    }

    // The netlist without its configuration lines: every searched dimension open.
    std::string open(std::string text)
    {
        for (const char* line : {".oversample 4 iir blocks=auto\n", ".adaa off\n", ".adaa on\n", ".solver omega(2)\n",
                                 ".solver omega(1)\n"})
        {
            const std::size_t at = text.find(line);
            if (at != std::string::npos)
                text.erase(at, std::string(line).size());
        }
        return text;
    }

    // Yeh's RC clipper with one pot in its series resistance: a small netlist to run the self-test on.
    const std::string kPotClipper = ".title PotClipper\n"
                                    ".input u in\n"
                                    ".output y a\n"
                                    ".pot drive 0.5\n"
                                    ".target level=4.5V\n"
                                    "R1 in a {1k + 4k * drive}\n"
                                    "C1 a 0 10n\n"
                                    "D1 a 0 DM\n"
                                    "D2 0 a DM\n"
                                    ".model DM D(IS=2.52n N=1.752)\n";

    // Yeh's RC clipper alone at L = 4.5 V, and behind an ideal x10 non-inverting stage at L = 0.45 V: the clipper sees
    // the same 4.5 V.
    const std::string kClipper45 = ".title Clip\n"
                                   ".input u in\n"
                                   ".output y a\n"
                                   ".oversample 4\n"
                                   ".target level=4.5V\n"
                                   "R1 in a 2.2k\n"
                                   "C1 a 0 10n\n"
                                   "D1 a 0 DM\n"
                                   "D2 0 a DM\n"
                                   ".model DM D(IS=2.52n N=1.752)\n";
    const std::string kBoosted = ".title Boosted\n"
                                 ".input u n\n"
                                 ".output y a\n"
                                 ".oversample 4\n"
                                 ".target level=0.45V\n"
                                 "X1 n m o opamp\n"
                                 "Rf o m 9k\n"
                                 "Rg m 0 1k\n"
                                 "R1 o a 2.2k\n"
                                 "C1 a 0 10n\n"
                                 "D1 a 0 DM\n"
                                 "D2 0 a DM\n"
                                 ".model DM D(IS=2.52n N=1.752)\n";

    // Each "self-test candidate k: ..." note's T1, in dB.
    std::vector<double> t1Figures(const std::string& notes)
    {
        std::vector<double> out;
        for (std::size_t at = notes.find("self-test candidate "); at != std::string::npos;
             at = notes.find("self-test candidate ", at + 1))
        {
            const std::size_t t1 = notes.find(": T1 ", at);
            REQUIRE(t1 != std::string::npos);
            out.push_back(std::stod(notes.substr(t1 + 5)));
        }
        return out;
    }

    // A shape that measures in a fraction of the time: a 1024-sample window (odd bins 3, 21 and 65: 141, 984 and 3047
    // Hz) and a 16x reference. The code is the same; the figures are not the report's.
    fc::StressShape small()
    {
        fc::StressShape s;
        s.window = 1024;
        s.warm = 256;
        s.bins = {3, 21, 65};
        s.referenceFactor = 16;
        return s;
    }

    // SelfTestOptions::parallel on `threads` std::threads.
    std::function<void(int, const std::function<void(int)>&)> pool(int threads)
    {
        return [threads](int count, const std::function<void(int)>& job)
        {
            std::atomic<int> next{0};
            auto work = [&]
            {
                for (int i = next++; i < count; i = next++)
                    job(i);
            };
            std::vector<std::thread> ts;
            for (int k = 1; k < threads; ++k)
                ts.emplace_back(work);
            work();
            for (std::thread& t : ts)
                t.join();
        };
    }

    // A verdict that passes (or not), for stub measures: each gate measured on one unit.
    fc::Verdict verdict(bool passes, double excess = 1)
    {
        fc::Verdict v;
        v.t1.max(passes ? -80 : -60 + excess, "a");
        v.t2.max(-80, "b");
        v.t3.max(0.1, "c");
        return v;
    }
} // namespace

TEST_CASE("the self-test's candidates, cheapest first", "[circuit]")
{
    // diodetone.cir's one EA port block, every dimension open: report §6.2's 16, in §3.2's model order
    const fc::Plan p = planned(open(fc::test::kDiodeTone));
    REQUIRE(p.config.oversampleOpen);
    REQUIRE(p.config.adaaOpen);
    REQUIRE(p.config.solverOpen);
    std::vector<std::string> got;
    for (const fc::Candidate& c : fc::candidates(p))
        got.push_back(std::format("{} {:g}", c.text(), c.cost));
    const std::vector<std::string> expected{
        "1x, omega(1) 154",  "1x, ADAA, omega(1) 211",  "1x, omega(2) 224",  "1x, ADAA, omega(2) 281",
        "2x, omega(1) 332",  "2x, ADAA, omega(1) 446",  "2x, omega(2) 472",  "2x, ADAA, omega(2) 586",
        "4x, omega(1) 688",  "4x, ADAA, omega(1) 916",  "4x, omega(2) 968",  "4x, ADAA, omega(2) 1196",
        "8x, omega(1) 1400", "8x, ADAA, omega(1) 1856", "8x, omega(2) 1960", "8x, ADAA, omega(2) 2416",
    };
    CHECK(got == expected);
    // a pinned dimension leaves the others: .oversample 4 gives the four 4x candidates
    const fc::Plan four = planned(replaced(open(fc::test::kDiodeTone), ".target", ".oversample 4\n.target"));
    CHECK(!four.config.oversampleOpen);
    REQUIRE(fc::candidates(four).size() == 4);
    CHECK(fc::candidates(four)[1].text() == "4x, ADAA, omega(1)");
    // and a netlist that names its whole configuration has none
    CHECK(fc::candidates(planned(fc::test::kDiodeTone)).empty());

    // within 5 % of the cheapest of a group, ADAA goes first, then fewer steps
    std::vector<fc::Candidate> list{{4, false, {fc::Solver::Guess::Omega, 2}, fc::Method::Tr, 1000},
                                    {4, false, {fc::Solver::Guess::Omega, 1}, fc::Method::Tr, 1040},
                                    {4, true, {fc::Solver::Guess::Omega, 2}, fc::Method::Tr, 1049},
                                    {8, false, {fc::Solver::Guess::Omega, 1}, fc::Method::Tr, 1051},
                                    {2, true, {fc::Solver::Guess::Omega, 1}, fc::Method::Tr, 900}};
    fc::orderCandidates(list);
    std::vector<std::string> order;
    for (const fc::Candidate& c : list)
        order.push_back(std::format("{} {:g}", c.text(), c.cost));
    CHECK(order == std::vector<std::string>{"2x, ADAA, omega(1) 900", "4x, ADAA, omega(2) 1049", "4x, omega(1) 1040",
                                            "4x, omega(2) 1000", "8x, omega(1) 1051"});
}

TEST_CASE("the ordered search stops at the first that passes", "[circuit]")
{
    const std::vector<fc::Candidate> list = fc::candidates(planned(open(fc::test::kDiodeTone)));
    REQUIRE(list.size() == 16);
    int measured = 0;
    auto stub = [&](const fc::Candidate& c) -> std::optional<fc::Verdict>
    {
        const auto index = static_cast<std::size_t>(&c - list.data());
        ++measured;
        return verdict(index == 3 || index == 5);
    };
    const fc::SearchOutcome found = fc::search(list, stub, {}, false);
    CHECK(found.chosen == 3);
    CHECK(found.met);
    CHECK(measured == 4);   // nothing after the first that passes
    CHECK(found.tried.size() == 4);
    // every candidate, for tables: the answer is still the first that passes
    measured = 0;
    const fc::SearchOutcome every = fc::search(list, stub, {}, true);
    CHECK(measured == 16);
    CHECK(every.chosen == 3);
    // a measure that stops (cancelled, over budget) stops the search, with no answer
    const fc::SearchOutcome stopped = fc::search(
        list, [&](const fc::Candidate& c) -> std::optional<fc::Verdict>
        { return &c - list.data() == 2 ? std::nullopt : std::optional<fc::Verdict>(verdict(false)); },
        {}, false);
    CHECK(stopped.stopped);
    CHECK(stopped.tried.size() == 2);
    // cancelled before any candidate: none measured
    measured = 0;
    CHECK(fc::search(list, stub, [] { return true; }, false).stopped);
    CHECK(measured == 0);
}

TEST_CASE("no configuration: the most accurate under the cap", "[circuit]")
{
    const std::vector<fc::Candidate> list = fc::candidates(planned(open(fc::test::kDiodeTone)));
    // nothing passes; each misses by less as the candidates cost more, until the cap (3,500 model cycles)
    std::vector<fc::Tried> tried;
    for (std::size_t i = 0; i < list.size(); ++i)
        tried.push_back({list[i], verdict(false, 10 - static_cast<double>(i) * 0.5)});
    const int best = fc::noConfig(tried);
    REQUIRE(best == 15);   // 8x ADAA omega(2), 2416 cycles: the least excess, and under the cap
    CHECK(tried[static_cast<std::size_t>(best)].candidate.cost < fc::kCostCap);
    // over the cap is never chosen
    tried[15].candidate.cost = 3600;
    CHECK(fc::noConfig(tried) == 14);
    // a candidate that is not finite somewhere is never chosen, however close
    tried[14].verdict.finite = false;
    CHECK(fc::noConfig(tried) == 13);
    // ties go to the higher oversampling
    tried[1].verdict = tried[13].verdict;
    CHECK(fc::noConfig(tried) == 13);
    tried[13].verdict.unconverged = 4;   // and an unconverged reference is not a measurement
    CHECK(fc::noConfig(tried) == 1);
    // the search falls back to it, met false (with the candidates' own costs, 8x ADAA omega(2) is under the cap)
    const fc::SearchOutcome found = fc::search(
        list, [&](const fc::Candidate& c) -> std::optional<fc::Verdict>
        { return tried[static_cast<std::size_t>(&c - list.data())].verdict; },
        {}, false);
    CHECK(!found.met);
    CHECK(found.tried.size() == 16);
    CHECK(found.chosen == 15);
}

TEST_CASE("the self-test's key", "[circuit]")
{
    const std::string base = open(fc::test::kDiodeTone);
    const std::string key = fc::selfTestKey(base, "diodetone.cir", {});
    REQUIRE(key.size() == 16);
    // the version is part of what it hashes
    fc::Messages messages;
    CHECK(fc::keyText(fc::parse(base, messages), {}).rfind(
              "fminus-circuit-selftest " + std::to_string(fc::kSelfTestVersion) + "\n", 0) == 0);
    // unchanged by what does not change the answer: comments, spacing, case, continuations, .title, .channels, .op
    for (const std::string& same : {
             replaced(base, "R1     in a  2.2k", "* a comment\nR1 in a 2.2k   ; and one at the end"),
             replaced(base, "R1     in a  2.2k", "r1 IN A 2.2K"),
             replaced(base, "Ptone  c  b  25k {1 - tone}", "Ptone c b 25k {1-tone}"),
             replaced(base, "Ptone  c  b  25k {1 - tone}", "Ptone c b\n+ 25k {1 - tone}"),
             replaced(base, ".title DiodeTone", ".title Other"),
             replaced(base, ".title DiodeTone", ".title DiodeTone\n.channels 2\n.op"),
             replaced(base, "N=1.752)", "N = 1.752)"),
             // a .model card's parentheses and commas separate, as the parser reads them
             replaced(base, "D(IS=2.52n N=1.752)", "D (IS=2.52n  N=1.752 )"),
             replaced(base, "D(IS=2.52n N=1.752)", "d( is=2.52n, n=1.752)"),
             replaced(base, "D(IS=2.52n N=1.752)", "D IS=2.52n N=1.752"),
             base + "\n\n",
         })
    {
        INFO(same);
        CHECK(fc::selfTestKey(same, "diodetone.cir", {}) == key);
    }
    // changed by what does
    for (const std::string& other : {
             replaced(base, "R1     in a  2.2k", "R1     in a  2.3k"),
             replaced(base, ".target", ".oversample 4\n.target"),
             replaced(base, "N=1.752", "N=1.75"),
             replaced(base, ".pot    tone 0.5 lin", ".pot    tone 0.6 lin"),
             replaced(base, "level=4.5V", "level=4V"),
         })
    {
        INFO(other);
        CHECK(fc::selfTestKey(other, "diodetone.cir", {}) != key);
    }
    fc::Options halfband;
    halfband.halfband = fc::Halfband::Fir;
    CHECK(fc::selfTestKey(base, "diodetone.cir", halfband) != key);
    fc::Options adaa;
    adaa.adaa = true;
    CHECK(fc::selfTestKey(base, "diodetone.cir", adaa) != key);
    // but not by what does not: the module's name, the channels, an answer
    fc::Options named;
    named.moduleName = "Other";
    named.channels = 2;
    named.chosen = fc::Choice{};
    CHECK(fc::selfTestKey(base, "diodetone.cir", named) == key);
    // the file's name is not in it either; a netlist with errors has none
    CHECK(fc::selfTestKey(base, "elsewhere/pedal.cir", {}) == key);
    CHECK(fc::selfTestKey(".input u in\n", "t.cir", {}).empty());
}

TEST_CASE("a Choice's text, and what generate makes of one", "[circuit]")
{
    const std::string base = open(fc::test::kDiodeTone);
    const fc::Plan p = planned(base);
    // parse(text()) gives every candidate back, and its figures exactly
    for (const fc::Candidate& c : fc::candidates(p))
    {
        for (bool met : {true, false})
        {
            fc::Verdict v = verdict(met);
            v.t1.value = met ? -93.12345678901234 : -50.12345678901234;
            v.t1.where = "4.5 V, 105 Hz, tone 0";
            v.t3.value = std::nextafter(0.1, 1.0);
            const fc::Choice choice = fc::choiceOf(p.key, c, v, met);
            std::string why;
            const std::optional<fc::Choice> back = fc::Choice::parse(choice.text(), &why);
            INFO(choice.text());
            INFO(why);
            REQUIRE(back);
            CHECK(back->text() == choice.text());
            CHECK(back->key == p.key);
            CHECK(back->oversample == c.oversample);
            CHECK(back->adaa == c.adaa);
            CHECK(back->solver.steps == c.solver.steps);
            CHECK(back->met == met);
            CHECK(back->t1 == v.t1.value);
            CHECK(back->t3 == v.t3.value);
            CHECK(back->worst == (met ? "" : "T1 -50.1 dB at 4.5 V, 105 Hz, tone 0"));
        }
    }
    fc::Choice four;
    four.key = p.key;
    four.oversample = 4;
    four.adaa = true;
    four.solver = {fc::Solver::Guess::Omega, 1};
    four.t1 = -78.7;
    four.t2 = -73.5;
    four.t3 = 0.14;
    CHECK(four.text() == "fminus-circuit-choice 1 key=" + p.key +
                             " oversample=4 adaa=on solver=omega(1) method=tr met=yes t1=-78.7 t2=-73.5 t3=0.14");
    // another version's, or not one at all, is read as nothing, with why
    std::string why;
    std::string text = four.text();
    CHECK(!fc::Choice::parse(replaced(text, "choice 1 ", "choice 2 "), &why));
    CHECK(why == "it is self-test version 2's; this one's is 1");
    CHECK(!fc::Choice::parse("fminus-circuit-choice 1 key=" + p.key + " oversample=3", &why));
    CHECK(why == "'oversample=3' is not a valid oversample (1, 2, 4 or 8)");
    CHECK(!fc::Choice::parse(replaced(text, " t3=0.14", ""), &why));
    CHECK(why == "it has no t3=");
    CHECK(!fc::Choice::parse("oversample=4", &why));
    CHECK(why == "not a self-test's answer (it begins 'fminus-circuit-choice')");

    // generate with a matching answer: not provisional, the configuration from the self-test
    fc::Options options;
    options.chosen = four;
    const fc::Result r = fc::generate(base, "diodetone.cir", options);
    INFO(r.errors);
    REQUIRE(r.errors.empty());
    CHECK(!r.provisional);
    CHECK(r.selfTestKey == p.key);
    CHECK(r.configuration == "configuration: .oversample 4, .adaa on, .solver omega(1) (the self-test " + p.key +
                                 "); iir blocks=auto, .method tr (default)");
    CHECK(r.notes.find("provisional") == std::string::npos);
    CHECK(r.warnings.empty());
    // without one: provisional
    const fc::Result provisional = fc::generate(base, "diodetone.cir");
    CHECK(provisional.provisional);
    CHECK(provisional.selfTestKey == p.key);
    CHECK(provisional.configuration == "configuration: .oversample 8, .adaa off, .solver omega(2) (provisional); iir "
                                       "blocks=auto, .method tr (default)");
    // another netlist's answer is ignored, with a note
    fc::Choice stale = four;
    stale.key = "0123456789abcdef";
    options.chosen = stale;
    const fc::Result ignored = fc::generate(base, "diodetone.cir", options);
    CHECK(ignored.provisional);
    CHECK(ignored.notes.find("diodetone.cir: note: Options::chosen is another netlist's or other Options' (key "
                             "0123456789abcdef; this one's is " +
                             p.key + "): it is ignored\n") != std::string::npos);
    // an answer fills only the open dimensions: an explicit .oversample 2 stays 2 under a chosen 8x
    const std::string two = replaced(base, ".target", ".oversample 2\n.target");
    fc::Choice eight = four;
    eight.key = fc::selfTestKey(two, "diodetone.cir", {});
    eight.oversample = 8;
    options.chosen = eight;
    const fc::Result kept = fc::generate(two, "diodetone.cir", options);
    REQUIRE(kept.errors.empty());
    CHECK(kept.configuration.rfind(
              "configuration: .oversample 2 (the netlist); .adaa on, .solver omega(1) (the self-test", 0) == 0);
    CHECK(kept.source.find("at 2 rate") != std::string::npos);
    // a malformed answer is an error of the Options
    fc::Choice bad = four;
    bad.key = "xyz";
    options.chosen = bad;
    CHECK(fc::generate(base, "diodetone.cir", options).errors ==
          "diodetone.cir:1:1: error: Options::chosen's key 'xyz' is not 16 hex digits\n");
}

TEST_CASE("an answer that meets nothing warns at the region's first port", "[circuit]")
{
    const std::string base = open(fc::test::kScreamer);
    const fc::Plan p = planned(base);
    fc::Choice choice;
    choice.key = p.key;
    choice.oversample = 8;
    choice.adaa = true;
    choice.solver = {fc::Solver::Guess::Omega, 2};
    choice.met = false;
    choice.worst = "T2 -55.1 dB at 2.2 V, 3012 Hz, drive 1";
    fc::Options options;
    options.chosen = choice;
    const fc::Result r = fc::generate(base, "pedal.cir", options);
    REQUIRE(r.errors.empty());
    // D1 is line 20 of kScreamer, 17 without its three configuration lines
    CHECK(r.warnings == "pedal.cir:17:1: warning: block 2 (D1, D2): no configuration meets the targets (worst: T2 "
                        "-55.1 dB at 2.2 V, 3012 Hz, drive 1); using the most accurate under the cost cap: 8x, ADAA, "
                        "omega(2)\n");
}

TEST_CASE("the self-test runs the same on any number of threads", "[circuit]")
{
    fc::Options options;
    options.oversample = 4;   // four candidates
    fc::SelfTestOptions serial;
    const fc::SelfTestResult a = fc::selfTestWith(kPotClipper, "clip.cir", options, serial, small());
    INFO(a.errors);
    REQUIRE(a.errors.empty());
    REQUIRE(a.choice);
    fc::SelfTestOptions threaded;
    threaded.parallel = pool(8);
    const fc::SelfTestResult b = fc::selfTestWith(kPotClipper, "clip.cir", options, threaded, small());
    REQUIRE(b.choice);
    CHECK(a.choice->text() == b.choice->text());
    CHECK(a.notes == b.notes);
    CHECK(a.candidates == b.candidates);
    CHECK(a.key == fc::selfTestKey(kPotClipper, "clip.cir", options));
    CHECK(a.notes.find("clip.cir: note: self-test " + a.key + ": .adaa, .solver open; 3 points x 17 signals = 51 "
                       "units, L = 4.5 V (.target level); 4 candidates, cheapest first\n") == 0);
    CHECK(a.notes.find("clip.cir: note: the self-test chose .adaa ") != std::string::npos);
    // the answer generates a module that is not provisional
    options.chosen = a.choice;
    CHECK(!fc::generate(kPotClipper, "clip.cir", options).provisional);
}

TEST_CASE("a gate passes only where it was measured", "[circuit]")
{
    const fc::StressMatrix matrix = fc::stressMatrix(planned(kPotClipper), small());
    const int units = matrix.units();
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // every gate NaN (the output and its references silent): nothing measured, so nothing passes
    std::vector<fc::UnitVerdict> silent(static_cast<std::size_t>(units));
    for (fc::UnitVerdict& u : silent)
    {
        u.ran = true;
        u.t1 = u.t2 = u.t3 = nan;
    }
    const fc::Verdict none = fc::reduce(matrix, silent, 0);
    CHECK(none.t1.measured == 0);
    CHECK(!none.passes());
    CHECK(none.excess() == inf);
    CHECK(none.worst() == "T1 measured nowhere (every output silent)");
    // one unit's figures pass; a NaN elsewhere is no measurement, a +inf (silent against not) the worst
    std::vector<fc::UnitVerdict> some = silent;
    some[10].t1 = -80;
    some[10].t2 = -80;
    some[10].t3 = 0.1;
    CHECK(fc::reduce(matrix, some, 0).passes());
    some[11].t1 = inf;
    const fc::Verdict worst = fc::reduce(matrix, some, 0);
    CHECK(!worst.passes());
    CHECK(worst.t1.value == inf);
    CHECK(worst.t1.measured == 2);
    CHECK(worst.worst() == "T1 inf dB at " + matrix.where(11));
}

TEST_CASE(".target level is a positive voltage", "[circuit]")
{
    for (const char* level : {"level=0V", "level=-4.5V", "level=1e300T"})
    {
        INFO(level);
        fc::Messages messages;
        const std::string text = replaced(kPotClipper, "level=4.5V", level);
        fc::parse(text, messages);
        CHECK(messages.format("t.cir", fc::Severity::Error) ==
              std::format("t.cir:5:9: error: '{}': the level is a positive voltage\n", level));
    }
}

TEST_CASE("T1 runs the region on what the blocks before it give it", "[circuit]")
{
    // the x10 stage is block 1, before the region: the region's input is ten times the signal
    const fc::Plan boosted = planned(kBoosted);
    REQUIRE(boosted.regionFirst == 1);
    const fc::StressMatrix matrix = fc::stressMatrix(boosted, small());
    CHECK(matrix.upstream == 1);
    CHECK(matrix.level == 0.45);
    const int unit = 10;   // L, on bin 21
    REQUIRE(matrix.signal(unit).amp == 0.45);
    const std::vector<double> u = fc::signalSamples(matrix, matrix.signal(unit), 4, 4 * matrix.shape.host());
    const std::vector<double> in = fc::regionInput(boosted, matrix, unit, 4, {});
    REQUIRE(in.size() == u.size());
    double worst = 0;
    for (std::size_t i = 0; i < u.size(); ++i)
        worst = std::max(worst, std::fabs(in[i] - 10 * u[i]));
    CHECK(worst < 1e-12);
    // with the region first, it is the signal itself
    const fc::Plan alone = planned(kClipper45);
    const fc::StressMatrix single = fc::stressMatrix(alone, small());
    CHECK(single.upstream == 0);
    CHECK(fc::regionInput(alone, single, unit, 4, {}) ==
          fc::signalSamples(single, single.signal(unit), 4, 4 * single.shape.host()));
    // a candidate's step budget counts the upstream run
    CHECK(fc::stepsCandidate(matrix, 4) == static_cast<std::int64_t>(matrix.units()) * 3 * 4 *
                                               static_cast<std::int64_t>(matrix.shape.host()));
    CHECK(fc::stepsCandidate(single, 4) == static_cast<std::int64_t>(single.units()) * 2 * 4 *
                                               static_cast<std::int64_t>(single.shape.host()));

    // so the boosted clipper's candidates measure the clipper's T1 at 4.5 V, not at the 0.45 V of its input
    fc::SelfTestOptions every;
    every.everyCandidate = true;
    const fc::SelfTestResult a = fc::selfTestWith(kBoosted, "boosted.cir", {}, every, small());
    const fc::SelfTestResult b = fc::selfTestWith(kClipper45, "clip.cir", {}, every, small());
    INFO(a.errors << a.notes << b.errors << b.notes);
    const std::vector<double> ta = t1Figures(a.notes);
    const std::vector<double> tb = t1Figures(b.notes);
    REQUIRE(ta.size() == 4);
    REQUIRE(tb.size() == 4);
    for (std::size_t i = 0; i < ta.size(); ++i)
        CHECK(std::fabs(ta[i] - tb[i]) <= 0.15);
    REQUIRE(a.choice);
    REQUIRE(b.choice);
    CHECK(a.choice->adaa == b.choice->adaa);
    CHECK(a.choice->solver.steps == b.choice->solver.steps);
}

TEST_CASE("the self-test stops when cancelled, and within its budget", "[circuit]")
{
    fc::Options options;
    options.oversample = 4;
    // cancelled before it starts: nothing runs
    fc::SelfTestOptions before;
    int batches = 0;
    before.parallel = [&](int count, const std::function<void(int)>& job)
    {
        ++batches;
        for (int i = 0; i < count; ++i)
            job(i);
    };
    before.cancelled = [] { return true; };
    const fc::SelfTestResult r = fc::selfTestWith(kPotClipper, "clip.cir", options, before, small());
    CHECK(r.cancelled);
    CHECK(!r.choice);
    CHECK(r.candidates == 0);
    CHECK(batches == 0);
    CHECK(r.notes.find("clip.cir: note: the self-test was cancelled; no answer\n") != std::string::npos);
    // cancelled once the references are done: no candidate's units run (the parallel batches are counted)
    std::atomic<bool> stop{false};
    fc::SelfTestOptions after;
    batches = 0;
    after.parallel = [&](int count, const std::function<void(int)>& job)
    {
        ++batches;
        for (int i = 0; i < count; ++i)
            job(i);
        stop = true;   // the first batch is the 64x references
    };
    after.cancelled = [&] { return stop.load(); };
    const fc::SelfTestResult late = fc::selfTestWith(kPotClipper, "clip.cir", options, after, small());
    CHECK(late.cancelled);
    CHECK(!late.choice);
    CHECK(batches == 1);
    CHECK(late.candidates == 0);
    // over budget: no answer, and the note says where it stopped
    fc::SelfTestOptions tiny;
    tiny.budget = 1000;
    const fc::SelfTestResult poor = fc::selfTestWith(kPotClipper, "clip.cir", options, tiny, small());
    CHECK(!poor.cancelled);
    CHECK(!poor.choice);
    const fc::StressMatrix matrix = fc::stressMatrix(planned(kPotClipper, options), small());
    CHECK(poor.notes.find(std::format("clip.cir: note: the self-test stops before the 16x references: it needs {} "
                                      "steps more, and 0 of its budget of 1000 are spent; no answer\n",
                                      fc::steps64(matrix))) != std::string::npos);
    // a netlist that names its whole configuration has nothing to choose
    const fc::SelfTestResult pinned = fc::selfTest(fc::test::kDiodeTone, "diodetone.cir", {});
    CHECK(!pinned.choice);
    CHECK(pinned.key.size() == 16);
    CHECK(pinned.notes == "diodetone.cir: note: self-test " + pinned.key +
                              ": the netlist names its whole configuration (or has no port): nothing to choose\n");
}

TEST_CASE("a job's exception comes back to the self-test's caller", "[circuit]")
{
    // a pool with no try/catch that runs every job on threads of its own; a job throws there (through `cancelled`,
    // which the jobs poll, as std::bad_alloc would from a unit's buffers): the test returns an internal error
    fc::Options options;
    options.oversample = 4;
    fc::SelfTestOptions test;
    test.parallel = [](int count, const std::function<void(int)>& job)
    {
        std::atomic<int> next{0};
        std::vector<std::thread> ts;
        for (int k = 0; k < 2; ++k)
            ts.emplace_back(
                [&]
                {
                    for (int i = next++; i < count; i = next++)
                        job(i);
                });
        for (std::thread& t : ts)
            t.join();
    };
    const std::thread::id caller = std::this_thread::get_id();
    test.cancelled = [caller]
    {
        if (std::this_thread::get_id() != caller)
            throw std::runtime_error("a job failed");
        return false;
    };
    const fc::SelfTestResult r = fc::selfTestWith(kPotClipper, "clip.cir", options, test, small());
    CHECK(r.errors == "clip.cir:1:1: error: internal error: a job failed\n");
    CHECK(!r.choice);
    CHECK(!r.cancelled);
    CHECK(r.key == fc::selfTestKey(kPotClipper, "clip.cir", options));
}

TEST_CASE("the guard's names are the module's", "[circuit]")
{
    for (const char* name : {"ck_", "fd_", "fdstep_", "sum_", "gain_"})
    {
        INFO(name);
        CHECK(fc::isModuleWord(name));
    }
}

namespace
{
    // otalp.cir without its configuration lines
    std::string openOta(std::string text)
    {
        for (const char* line : {".oversample 4 iir blocks=auto\n", ".adaa off\n", ".solver newton(1)\n"})
            text = replaced(text, line, "");
        return text;
    }
} // namespace

TEST_CASE("the smooth-monotone row, cheapest first", "[circuit]")
{
    // otalp.cir's two OTA port blocks, every dimension open: report §6.2's SM row, 1, 2 and 4x, ADAA off and on, the
    // previous v + 1, 2 or 3 Newton steps; a block a step costs 10 + 2 (p), 5 (the guess), 60 a Newton step, 100 for
    // ADAA's tail and 5 (the update), and the halfbands 12 a stage-sample
    const fc::Plan p = planned(openOta(fc::test::kOtaLp));
    REQUIRE(p.smooth);
    REQUIRE(p.config.provisional);
    CHECK(p.config.oversample == 2);
    CHECK(p.config.solver.guess == fc::Solver::Guess::Previous);
    CHECK(p.config.solver.steps == 2);
    CHECK(!p.config.adaa);
    std::vector<std::string> got;
    for (const fc::Candidate& c : fc::candidates(p))
        got.push_back(std::format("{} {:g}", c.text(), c.cost));
    const std::vector<std::string> expected{
        "1x, newton(1) 164",        "1x, newton(2) 284",        "1x, ADAA, newton(1) 364",  "2x, newton(1) 352",
        "1x, newton(3) 404",        "1x, ADAA, newton(2) 484",  "1x, ADAA, newton(3) 604",  "2x, newton(2) 592",
        "2x, ADAA, newton(1) 752",  "4x, newton(1) 728",        "2x, newton(3) 832",        "2x, ADAA, newton(2) 992",
        "2x, ADAA, newton(3) 1232", "4x, newton(2) 1208",       "4x, ADAA, newton(1) 1528", "4x, newton(3) 1688",
        "4x, ADAA, newton(2) 2008", "4x, ADAA, newton(3) 2488",
    };
    CHECK(got == expected);
    // the provisional note names it
    const fc::Result r = fc::generate(openOta(fc::test::kOtaLp), "otalp.cir");
    CHECK(r.provisional);
    CHECK(r.notes.find("otalp.cir: note: the self-test chooses this circuit's configuration; provisional: 2x IIR, "
                       "newton(2)") != std::string::npos);
}

TEST_CASE("law inputs are stress axes, with a synth's signals", "[circuit]")
{
    const fc::Plan p = planned(fc::test::kOtaLp);
    const fc::StressMatrix m = fc::stressMatrix(p);
    // the law input's low end, geometric middle and high end
    REQUIRE(m.points.size() == 3);
    const double mid = std::sqrt(1e-5 * 2e-3);
    CHECK(std::fabs(m.points[0].laws[0] - 1e-5) <= 1e-18);
    CHECK(std::fabs(m.points[1].laws[0] - mid) <= 1e-15);
    CHECK(std::fabs(m.points[2].laws[0] - 2e-3) <= 1e-15);
    CHECK(m.points[1].label == "iabc 0.000141");
    // 15 sines, the sweep, the noise, the saw (T1, T2, T4) and iabc's modulation (T1, T4)
    REQUIRE(m.signals.size() == 19);
    const fc::StressSignal& saw = m.signals[17];
    CHECK(saw.kind == fc::StressSignal::Kind::Saw);
    CHECK(saw.t1);
    CHECK(saw.t2);
    CHECK(!saw.t3);
    const fc::StressSignal& mod = m.signals[18];
    CHECK(mod.kind == fc::StressSignal::Kind::Modulated);
    CHECK(mod.t1);
    CHECK(!mod.t2);
    CHECK(!mod.t3);
    CHECK(mod.name == "996 Hz at 5 V, iabc +-2 octaves at 37 Hz");
    // the modulation runs where iabc is at its middle, around which it moves +-2 octaves at 37 Hz, held over a host
    // sample's steps
    CHECK(!m.runs(18));
    CHECK(m.runs(19 + 18));
    CHECK(!m.runs(38 + 18));
    const std::vector<double> held = fc::lawSamples(p, m, 19 + 18, 4, 4 * 4800);
    double lo = 1;
    double hi = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
    {
        lo = std::min(lo, held[i]);
        hi = std::max(hi, held[i]);
        if (i % 4 != 0)
            CHECK(held[i] == held[i - 1]);
    }
    CHECK(std::fabs(hi / mid - 4) <= 1e-3);
    CHECK(std::fabs(mid / lo - 4) <= 1e-3);
    // a cv input from 0: its middle is the arithmetic one, and it moves over its whole range
    const std::string cv =
        replaced(fc::test::kOtaLp, ".input  iabc -    kind=current range=10u:2m", ".input iabc - kind=cv scale=2m");
    const fc::Plan pc = planned(cv);
    const fc::StressMatrix mc = fc::stressMatrix(pc);
    CHECK(mc.points[1].laws[0] == 0.5);
    const std::vector<double> sweep = fc::lawSamples(pc, mc, 19 + 18, 1, 4800);
    CHECK(*std::min_element(sweep.begin(), sweep.end()) < 1e-3);
    CHECK(*std::max_element(sweep.begin(), sweep.end()) > 0.999);
    // a law input's range is part of the self-test's key
    const std::string other = replaced(openOta(fc::test::kOtaLp), "range=10u:2m", "range=10u:1m");
    CHECK(fc::selfTestKey(openOta(fc::test::kOtaLp), "t.cir", {}) != fc::selfTestKey(other, "t.cir", {}));
}

TEST_CASE("an OTA stage's self-test chooses from the SM row, the same on any number of threads", "[circuit]")
{
    const std::string text = fc::test::otaStage(".target level=5V\n");
    const fc::SelfTestResult one = fc::selfTestWith(text, "t.cir", {}, {}, small());
    INFO(one.errors << one.notes);
    REQUIRE(one.choice);
    CHECK(one.choice->solver.guess == fc::Solver::Guess::Previous);
    CHECK(one.choice->oversample <= 4);
    CHECK(one.notes.find("self-test candidate 1: 1x, newton(1), 82 model cycles") != std::string::npos);
    fc::SelfTestOptions test;
    test.parallel = pool(4);
    const fc::SelfTestResult four = fc::selfTestWith(text, "t.cir", {}, test, small());
    REQUIRE(four.choice);
    CHECK(four.choice->text() == one.choice->text());
    CHECK(four.notes == one.notes);
    // and the module its answer makes is one generate() takes back, for the key it names
    fc::Options o;
    o.chosen = one.choice;
    const fc::Result r = fc::generate(text, "t.cir", o);
    CHECK(r.errors.empty());
    CHECK(!r.provisional);
    CHECK(r.configuration.find("(the self-test " + one.key + ")") != std::string::npos);
}
