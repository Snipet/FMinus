// src/ir/Within.cpp: `within (milliseconds) { ... }`, a deadline scope for the web client's requests (docs/web.md,
// "within"), in a build with FMINUS_WEB. Its runtime is the web library's (src/web/Web.h, "deadline scopes"): a scope of
// the calling thread under the program's context, which the requests it starts join and its waits read.
//
// The lowering is defer's (Statements.cpp): the milliseconds computed once, the scope begun, and its end a deferred
// action of a block of its own around the within's braces, so that every way out of the block (its end, return, break,
// continue, a reply) ends it exactly once, after the block's own deferred statements have run:
//
//     %ms = ...
//     %web.context = load ptr, ptr @fminus_web_context, align 8
//     %within.begun = call i32 @fminus_web_http_within_begin(ptr %web.context, i32 %ms)
//     %within.open = icmp eq i32 %within.begun, 0          ; no memory: no scope, and none to end
//     store i1 %within.open, ptr %within.slot
//     ...                                                  ; the block
//     %within.opened = load i1, ptr %within.slot           ; at each way out
//     br i1 %within.opened, label %within.end, label %within.done
//   within.end:
//     %within.ended = call i32 @fminus_web_http_within_end(ptr %web.context2)
//     br label %within.done
//
// A within begun and never ended would stay its thread's (the library finds a scope by the thread's id): a call the
// loop budget or a bounds check stops leaves every block without reaching its end, so those passes abandon the
// thread's scopes on their way out of a function that begins one (opt/EarlyReturn.h). A switch may not jump into the
// block, past the begin (Statements.cpp's lowerLabel).
//
// The effects: it waits where it ends, as http_wait does, and its begin allocates, so a nonblocking function refuses it
// in http_wait's words and a nonallocating one as an allocation (Effects.cpp), each at the within. In a build without
// the web library it is the error http_wait is there, and its block is lowered plainly, for its own errors.
#include "ir/Lowering.h"

#include "fminus/Config.h"
#include "opt/EarlyReturn.h"

#include <string_view>

namespace fminus::ir::detail
{
    // The passes find a within's begin, and the context its abandon takes, by these names.
    static_assert(std::string_view(opt::kWebContextGlobal) == kWebContextSymbol);

    void Lowering::lowerWithin(SourceLocation location, const ast::WithinStmt& within)
    {
        const auto& block = std::get<ast::BlockStmt>(within.body->node);
        const auto milliseconds = [&] {
            const ast::Expr& timeout = *within.timeout;
            const Value value = isString(timeout) ? refuseString(timeout, ast::Type::Int, Use::Passing, begin(timeout))
                                                  : lowerAs(timeout, ast::Type::Int);
            return convert(value, ast::Type::Int, begin(timeout), Use::Passing);
        };
        if (!isBuilt(Feature::Web))
        {
            error(location, notBuilt("within", Feature::Web));
            milliseconds();   // for its own errors
            lowerBlock(block.items);
            return;
        }
        llvm::Value* timeout = milliseconds();
        calls_[function_->name].push_back({location, std::string(kWithinCall)});   // what Effects.cpp checks
        llvm::Value* context = webContext(location);
        llvm::CallInst* begun = callRuntime(opt::kWithinBeginSymbol, builder_.getInt32Ty(), {context, timeout});
        begun->setName("within.begun");
        llvm::AllocaInst* slot = allocate(builder_.getInt1Ty(), "within.slot");
        tag(builder_.CreateStore(builder_.CreateICmpEQ(begun, builder_.getInt32(0), "within.open"), slot),
            ast::Type::Bool);

        // Its end, a deferred action of a block around the braces: the block's own defers run first.
        openDefers();
        deferScopes_.back().reached.push_back(
            Deferred{nullptr, location, scopes_.size() - 1, {}, breakables_.size(), switches_.size(), slot});
        lowerBlock(block.items);
        closeDefers();
    }

    // A within's end, where its block is left: the scope ended when it was begun. What the end returns (whether time
    // or a stop ended some of its requests) is no value of the program's: each request's own status says it.
    void Lowering::lowerWithinEnd(const Deferred& end)
    {
        if (builder_.GetInsertBlock()->hasTerminator())   // after another's error (runDefers)
            builder_.SetInsertPoint(newBlock("unreachable"));
        llvm::LoadInst* open = builder_.CreateLoad(builder_.getInt1Ty(), end.within, "within.opened");
        tag(open, ast::Type::Bool);
        llvm::BasicBlock* endBlock = newBlock("within.end");
        llvm::BasicBlock* doneBlock = newBlock("within.done");
        builder_.CreateCondBr(open, endBlock, doneBlock);
        builder_.SetInsertPoint(endBlock);
        callRuntime("fminus_web_http_within_end", builder_.getInt32Ty(), {webContext(end.location)})
            ->setName("within.ended");
        builder_.CreateBr(doneBlock);
        builder_.SetInsertPoint(doneBlock);
    }
} // namespace fminus::ir::detail
