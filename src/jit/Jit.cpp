// src/jit/Jit.cpp
#include "jit/Jit.h"

#include "support/HostTarget.h"
#include "support/JitRuntime.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Target/TargetMachine.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace fminus::jit
{
    // Where each function's code is, written by the linker's plugin (JITLink) or listener (RuntimeDyld) on the thread
    // that links, and read by code() on any.
    struct CodeLog
    {
        char prefix = '\0';   // the target's global prefix, which an object's names have and the module's do not
        mutable std::mutex mutex;
        std::vector<Jit::Code> linked;
        std::map<const void*, std::vector<Jit::Code>> pending;   // a JITLink link's, until it is emitted (or fails)
        Jit::Inlined inlined;                                    // by function (Jit::recordInlining)

        // A symbol's name as its module has it, or nothing for one that is none of the module's: a name without the
        // prefix (an assembler's label, macOS's ltmp0), an ELF object's local label (.L) or mapping symbol ($x).
        std::optional<std::string> nameOf(llvm::StringRef symbol) const
        {
            if (prefix != '\0')
            {
                if (!symbol.consume_front(llvm::StringRef(&prefix, 1)))
                    return std::nullopt;
            }
            if (symbol.empty() || symbol.starts_with(".L") || symbol.starts_with("$"))
                return std::nullopt;
            return symbol.str();
        }

        void add(std::vector<Jit::Code> code)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            linked.insert(linked.end(), std::make_move_iterator(code.begin()), std::make_move_iterator(code.end()));
        }
    };

    namespace
    {
        bool executable(const llvm::jitlink::Symbol& symbol)
        {
            return (std::to_underlying(symbol.getSection().getMemProt())
                    & std::to_underlying(llvm::orc::MemProt::Exec))
                   != 0;
        }

        // Before JITLink prunes a graph: it removes each symbol that is not live, and keeps each block a live symbol
        // reaches. An ELF object's call of a local function (a parallel region's body, an entry point's) names its
        // section's symbol and an offset, .text + 0x1c0, not the function's, so the function's own symbol would go,
        // its code kept but its name lost. This marks live each named symbol of an executable section whose block the
        // link keeps, the blocks the live symbols reach, walked as prune() walks them: nothing is linked that would not
        // be.
        llvm::Error keepNamesOfKeptCode(llvm::jitlink::LinkGraph& graph)
        {
            std::vector<llvm::jitlink::Symbol*> pending;
            for (llvm::jitlink::Symbol* symbol : graph.defined_symbols())
            {
                if (symbol->isLive())
                    pending.push_back(symbol);
            }
            llvm::DenseSet<const llvm::jitlink::Block*> kept;
            while (!pending.empty())
            {
                llvm::jitlink::Block& block = pending.back()->getBlock();
                pending.pop_back();
                if (!kept.insert(&block).second)
                    continue;
                for (llvm::jitlink::Edge& edge : block.edges())
                {
                    if (edge.getTarget().isDefined())
                        pending.push_back(&edge.getTarget());
                }
            }
            for (llvm::jitlink::Symbol* symbol : graph.defined_symbols())
            {
                if (symbol->hasName() && executable(*symbol) && kept.contains(&symbol->getBlock()))
                    symbol->setLive(true);
            }
            return llvm::Error::success();
        }

        // JITLink's: each object's graph, once its code has its addresses and every fix-up is applied (a post-fix-up
        // pass), written down as pending, and kept when the link is emitted. Code stays until the JIT ends, so there is
        // nothing to remove.
        class LinkedCode final : public llvm::orc::LinkGraphLinkingLayer::Plugin
        {
        public:
            explicit LinkedCode(std::shared_ptr<CodeLog> log) : log_(std::move(log)) {}

            void modifyPassConfig(llvm::orc::MaterializationResponsibility& responsibility, llvm::jitlink::LinkGraph&,
                                  llvm::jitlink::PassConfiguration& config) override
            {
                config.PrePrunePasses.push_back(keepNamesOfKeptCode);
                config.PostFixupPasses.push_back(
                    [log = log_, key = static_cast<const void*>(&responsibility)](llvm::jitlink::LinkGraph& graph) {
                        std::vector<Jit::Code> code;
                        for (llvm::jitlink::Symbol* symbol : graph.defined_symbols())
                        {
                            if (!executable(*symbol) || !symbol->hasName() || symbol->getSize() == 0)
                                continue;
                            if (std::optional<std::string> name = log->nameOf(*symbol->getName()))
                                code.push_back({std::move(*name), symbol->getAddress().getValue(), symbol->getSize(),
                                                {}});
                        }
                        const std::lock_guard<std::mutex> lock(log->mutex);
                        log->pending[key] = std::move(code);
                        return llvm::Error::success();
                    });
            }

            llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility& responsibility) override
            {
                std::vector<Jit::Code> code;
                {
                    const std::lock_guard<std::mutex> lock(log_->mutex);
                    const auto found = log_->pending.find(&responsibility);
                    if (found == log_->pending.end())
                        return llvm::Error::success();
                    code = std::move(found->second);
                    log_->pending.erase(found);
                }
                log_->add(std::move(code));
                return llvm::Error::success();
            }

            llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility& responsibility) override
            {
                const std::lock_guard<std::mutex> lock(log_->mutex);
                log_->pending.erase(&responsibility);
                return llvm::Error::success();
            }

            llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override
            {
                return llvm::Error::success();
            }

            void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey,
                                             llvm::orc::ResourceKey) override
            {
            }

        private:
            std::shared_ptr<CodeLog> log_;
        };

        template <typename T>
        std::optional<T> valueOf(llvm::Expected<T> expected)
        {
            if (!expected)
            {
                llvm::consumeError(expected.takeError());
                return std::nullopt;
            }
            return std::move(*expected);
        }

        // RuntimeDyld's (Windows'): each object once it is loaded and finalised, its functions from its symbol table,
        // sized as object::computeSymbolSizes sizes them (by the next symbol where the format has no sizes, as COFF and
        // Mach-O), each at its section's load address plus its offset in the section.
        class LoadedCode final : public llvm::JITEventListener
        {
        public:
            explicit LoadedCode(std::shared_ptr<CodeLog> log) : log_(std::move(log)) {}

            void notifyObjectLoaded(ObjectKey, const llvm::object::ObjectFile& object,
                                    const llvm::RuntimeDyld::LoadedObjectInfo& loaded) override
            {
                std::vector<Jit::Code> code;
                for (const auto& [symbol, size] : llvm::object::computeSymbolSizes(object))
                {
                    const std::optional<llvm::object::SymbolRef::Type> type = valueOf(symbol.getType());
                    const std::optional<llvm::StringRef> name = valueOf(symbol.getName());
                    const std::optional<std::uint64_t> address = valueOf(symbol.getAddress());
                    const std::optional<llvm::object::section_iterator> section = valueOf(symbol.getSection());
                    if (!type || *type != llvm::object::SymbolRef::ST_Function || !name || !address || !section
                        || *section == object.section_end() || size == 0)
                        continue;
                    const std::uint64_t base = loaded.getSectionLoadAddress(**section);
                    std::optional<std::string> named = log_->nameOf(*name);
                    if (base == 0 || !named)
                        continue;
                    code.push_back({std::move(*named), base + (*address - (*section)->getAddress()), size, {}});
                }
                log_->add(std::move(code));
            }

        private:
            std::shared_ptr<CodeLog> log_;
        };
    } // namespace

    llvm::Expected<std::unique_ptr<Jit>> Jit::create(llvm::OptimizationLevel level, Modules modules, Linker linker)
    {
        const llvm::CodeGenOptLevel codegen = codeGenLevel(level);
        auto machine = hostTargetMachine(codegen);
        if (!machine)
            return machine.takeError();
        auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!builder)
            return builder.takeError();
        builder->setCodeGenOptLevel(codegen);
        const bool coff = builder->getTargetTriple().isOSBinFormatCOFF();
        llvm::orc::LLJITBuilder jitBuilder;
        jitBuilder.setJITTargetMachineBuilder(std::move(*builder));
        if (linker == Linker::RuntimeDyld)   // as LLJIT makes Windows' (LLJIT::createObjectLinkingLayer)
            jitBuilder.setObjectLinkingLayerCreator(
                [coff](llvm::orc::ExecutionSession& session,
                       llvm::jitlink::JITLinkMemoryManager&) -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                    auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                        session, [](const llvm::MemoryBuffer&) { return std::make_unique<llvm::SectionMemoryManager>(); });
                    if (coff)
                    {
                        layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
                        layer->setAutoClaimResponsibilityForObjectSymbols(true);
                    }
                    return std::unique_ptr<llvm::orc::ObjectLayer>(std::move(layer));
                });
        auto lljit = jitBuilder.create();
        if (!lljit)
            return lljit.takeError();

        std::unique_ptr<Jit> jit(new Jit());
        jit->level_ = level;
        jit->machine_ = std::move(*machine);
        jit->log_ = std::make_shared<CodeLog>();
        jit->log_->prefix = (*lljit)->getDataLayout().getGlobalPrefix();
        llvm::orc::ObjectLayer& layer = (*lljit)->getObjLinkingLayer();
        if (auto* linking = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(&layer))
            linking->addPlugin(std::make_shared<LinkedCode>(jit->log_));
        else if (auto* loading = llvm::dyn_cast<llvm::orc::RTDyldObjectLinkingLayer>(&layer))
        {
            jit->listener_ = std::make_unique<LoadedCode>(jit->log_);
            loading->registerJITEventListener(*jit->listener_);
        }
        jit->jit_ = std::move(*lljit);
        if (llvm::Error error = defineRuntimeSymbols(*jit->jit_))
            return error;
        if (modules == Modules::Optimised)
            return jit;   // LLJIT's transform layer, with no transform set, hands each module on as it is

        Jit* self = jit.get();
        jit->jit_->getIRTransformLayer().setTransform(
            [self](llvm::orc::ThreadSafeModule tsm,
                   llvm::orc::MaterializationResponsibility&) -> llvm::Expected<llvm::orc::ThreadSafeModule> {
                tsm.withModuleDo([self](llvm::Module& module) {
                    const Calls before = directCalls(module);
                    optimiseModule(module, *self->machine_, self->level_);
                    self->recordInlining(inlinedCalls(before, directCalls(module)));
                });
                return std::move(tsm);
            });
        return jit;
    }

    Jit::~Jit() = default;

    llvm::Error Jit::add(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::LLVMContext> context)
    {
        module->setDataLayout(jit_->getDataLayout());
        module->setTargetTriple(jit_->getTargetTriple());
        return jit_->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
    }

    llvm::Expected<llvm::orc::ExecutorAddr> Jit::lookup(llvm::StringRef name)
    {
        return jit_->lookup(name);
    }

    std::vector<Jit::Code> Jit::code() const
    {
        std::vector<Code> code;
        {
            const std::lock_guard<std::mutex> lock(log_->mutex);
            code = log_->linked;
            for (Code& function : code)
            {
                if (const auto found = log_->inlined.find(function.name); found != log_->inlined.end())
                    function.inlined = found->second;
            }
        }
        std::sort(code.begin(), code.end(), [](const Code& a, const Code& b) {
            return a.address != b.address ? a.address < b.address : a.name < b.name;
        });
        return code;
    }

    Jit::Calls Jit::directCalls(const llvm::Module& module)
    {
        Calls calls;
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;
            auto& callees = calls[function.getName().str()];
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    const llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
                    if (callee != nullptr && callee != &function && !callee->isDeclaration())
                        ++callees[callee->getName().str()];
                }
            }
        }
        return calls;
    }

    namespace
    {
        // The functions `before`'s calls reach from `function`, each after every function that calls it on the way
        // (a reverse post-order: a call back up the way, recursion, is the one it does not wait for), `function` first.
        std::vector<std::string_view> callersFirst(const Jit::Calls& before, std::string_view function)
        {
            struct Visit
            {
                std::string_view function;
                Jit::Calls::mapped_type::const_iterator next;
                Jit::Calls::mapped_type::const_iterator end;
            };
            std::vector<std::string_view> after;   // each function once all it calls are done
            std::set<std::string_view> seen{function};
            std::vector<Visit> visits;
            const auto visit = [&](std::string_view caller) {
                if (const auto calls = before.find(caller); calls != before.end())
                    visits.push_back({caller, calls->second.begin(), calls->second.end()});
                else
                    after.push_back(caller);
            };
            visit(function);
            while (!visits.empty())
            {
                Visit& top = visits.back();
                if (top.next == top.end)
                {
                    after.push_back(top.function);
                    visits.pop_back();
                    continue;
                }
                const std::string_view callee = (top.next++)->first;
                if (seen.insert(callee).second)
                    visit(callee);
            }
            std::reverse(after.begin(), after.end());
            return after;
        }

        // count + calls * sites, held at the largest size_t past it (a chain of inlined calls multiplies them).
        std::size_t plusCalls(std::size_t count, std::size_t calls, std::size_t sites)
        {
            constexpr std::size_t most = std::numeric_limits<std::size_t>::max();
            return sites != 0 && calls > (most - count) / sites ? most : count + calls * sites;
        }
    } // namespace

    // For each function F, a walk of `before`'s calls from it, each function after every one that calls it on the way,
    // counting the calls of each that F would make had nothing been inlined.
    Jit::Inlined Jit::inlinedCalls(const Calls& before, const Calls& after)
    {
        Inlined inlined;
        for (const auto& [function, calls] : after)
        {
            const auto was = before.find(function);
            const auto fewer = [&calls](const auto& callee) {
                const auto still = calls.find(callee.first);
                return still == calls.end() || still->second < callee.second;
            };
            if (was == before.end() || std::none_of(was->second.begin(), was->second.end(), fewer))
                continue;   // F makes each call it made: nothing was inlined into it, nor through what it calls
            std::map<std::string_view, std::size_t> expected(was->second.begin(), was->second.end());
            std::vector<std::string> found;
            for (const std::string_view callee : callersFirst(before, function))
            {
                const auto still = calls.find(callee);
                const std::size_t kept = still != calls.end() ? still->second : 0;
                const auto wanted = expected.find(callee);
                if (callee == function || wanted == expected.end() || wanted->second <= kept)
                    continue;
                const std::size_t sites = wanted->second - kept;
                found.emplace_back(callee);
                if (const auto further = before.find(callee); further != before.end())
                {
                    for (const auto& [next, count] : further->second)
                        expected[next] = plusCalls(expected[next], count, sites);
                }
            }
            if (!found.empty())
            {
                std::sort(found.begin(), found.end());
                inlined.emplace(function, std::move(found));
            }
        }
        return inlined;
    }

    void Jit::recordInlining(const Inlined& inlined)
    {
        const std::lock_guard<std::mutex> lock(log_->mutex);
        for (const auto& [function, callees] : inlined)
            log_->inlined.insert_or_assign(function, callees);
    }
} // namespace fminus::jit
