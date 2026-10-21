// src/ir/Effects.cpp: clang's function effects, [[clang::nonallocating]] and [[clang::nonblocking]], after a function's
// parameter list (C23's attribute syntax), checked as clang's -Wfunction-effects checks them, but as errors. They are
// what keeps the heap off the audio thread: a kernel marked nonblocking can call no malloc, however far down.
//
//   nonallocating   the function allocates and frees nothing: it calls no malloc, calloc or free, and makes and
//                   frees no FFT plan (fft_create, fft_create_complex, fft_destroy; a transform is fine).
//   nonblocking     and nothing else that might wait: no external function (a prototype the program does not
//                   define) unless its own prototype is marked nonblocking too.
//
// A call is fine when it goes to a builtin of <math.h> (as clang trusts its own), to a function marked with the effect
// (or the stronger one: nonblocking includes nonallocating), or to a function of the program, unmarked, whose body is
// fine by the same rule: clang infers the effect through calls whose definitions it sees, and here every definition
// is seen. An external function must say so in its prototype. A call that is not fine is an error at the call, and
// notes follow the calls down to what is wrong, as clang's notes do.
//
// lowerCall records every call as it lowers it (calls_); checkEffects runs once every body is lowered.
#include "ir/Lowering.h"

namespace fminus::ir::detail
{
    namespace
    {
        const char* effectName(Lowering::Effect effect)
        {
            return effect == Lowering::Effect::NonBlocking ? "nonblocking" : "nonallocating";
        }
    } // namespace

    // The effect a declaration states: the strongest of its attributes. An attribute FMinus does not know is an
    // error, where clang only warns that it ignores it.
    Lowering::Effect Lowering::declaredEffect(const ast::Function& function)
    {
        Effect effect = Effect::None;
        for (const ast::Attribute& attribute : function.attributes)
        {
            if (attribute.name == "clang::nonblocking")
                effect = Effect::NonBlocking;
            else if (attribute.name == "clang::nonallocating")
                effect = std::max(effect, Effect::NonAllocating);
            else
                error(attribute.location, "unknown attribute " + quoted(attribute.name)
                                              + ": FMinus has clang::nonallocating and clang::nonblocking");
        }
        return effect;
    }

    Lowering::Effect Lowering::effectOf(const std::string& name) const
    {
        const auto found = effects_.find(name);
        return found == effects_.end() ? Effect::None : found->second;
    }

    // Why a call of `callee` breaks `required`: the notes that explain it (none for an allocation builtin, which needs
    // no explaining), or nothing when the call is fine. A function already being looked into (recursion) adds nothing
    // the first look will not find.
    std::optional<std::vector<Diagnostic>> Lowering::unsafe(const std::string& callee, Effect required,
                                                             std::vector<std::string>& looking)
    {
        if (allocates(callee))
            return std::vector<Diagnostic>{};
        const auto found = functions_.find(callee);
        if (found == functions_.end() || effectOf(callee) >= required)
            return std::nullopt;   // undeclared (an error of its own), or marked: its own definition is checked
        const std::string effect = effectName(required);
        const ast::Function& function = *found->second;
        if (!function.isDefinition)
            return std::vector<Diagnostic>{{function.location,
                                            "declaration cannot be inferred '" + effect
                                                + "' because it has no definition in this program",
                                            {}}};
        if (std::ranges::find(looking, callee) != looking.end())
            return std::nullopt;
        looking.push_back(callee);
        std::optional<std::vector<Diagnostic>> notes;
        for (const CallSite& call : calls_[callee])
        {
            if (auto inner = unsafe(call.callee, required, looking))
            {
                notes = std::vector<Diagnostic>{{call.location,
                                                 "function cannot be inferred '" + effect
                                                     + "' because it calls non-'" + effect + "' function "
                                                     + quoted(call.callee),
                                                 {}}};
                notes->insert(notes->end(), inner->begin(), inner->end());
                break;
            }
        }
        looking.pop_back();
        return notes;
    }

    // Every call of every function marked with an effect, checked against it: an error at each call that breaks it.
    void Lowering::checkEffects(const ast::Program& program)
    {
        for (const ast::Function& function : program.functions)
        {
            const auto found = functions_.find(function.name);
            if (!function.isDefinition || found == functions_.end() || found->second != &function)
                continue;
            const Effect effect = effectOf(function.name);
            if (effect == Effect::None)
                continue;
            for (const CallSite& call : calls_[function.name])
            {
                std::vector<std::string> looking{function.name};
                if (std::optional<std::vector<Diagnostic>> notes = unsafe(call.callee, effect, looking))
                {
                    const std::string name = effectName(effect);
                    diagnostics_.error(call.location,
                                       "function with '" + name + "' attribute must not call non-'" + name
                                           + "' function " + quoted(call.callee),
                                       std::move(*notes));
                }
            }
        }
    }
} // namespace fminus::ir::detail
