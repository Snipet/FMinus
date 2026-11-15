// src/ir/Text.cpp: FMinus's type string (frontend/Ast.h), a read-only view of bytes something else owns: a pointer and
// a length, {ptr, i32} in LLVM, a value as a float is (a register pair, never an address), passed to and returned from
// a program's own functions as that two-element struct. In memory it is C's struct { const char* data; int size; }, 16
// bytes aligned to 8, loaded and stored as its two members, each with its own TBAA tag (Tbaa.cpp).
//
// A string literal is a string where one is wanted (lowerAs, refuseString): its bytes the private constant a builtin's
// literal is (Strings.cpp), no NUL after them, and their count. == and != compare two by content, the same length and
// the same bytes, through the runtime (fminus_rt_string_equal, runtime/Text.cpp); nothing else takes one. A switch on
// one compares its value with each case's literal in the labels' order and jumps to the first that is equal, a chain of
// those calls in place of LLVM's switch (WG14 N3895's semantics, with lengths for NULs).
//
// The string_* builtins (Calls.cpp's table) are wait-free: those that need no more than the pointer and the length are
// made here, inline; the rest are the runtime's. None allocates: text is read as a value here, and made only in sinks.
// No function of the runtime returns a string (C's ABI for a struct differs by platform); one that gives text gives
// its pointer and writes its length through a pointer, and the code here makes the pair.
//
// A char array keeps one: string_copy writes a string's bytes into it, string_of views them again, and C's
// `char name[8] = "abc";` copies a literal's bytes into it, the rest 0 (initializeChars).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

namespace fminus::ir::detail
{
    namespace
    {
        // A literal's bytes as C would spell them, in quotes, as a message shows a case's value.
        std::string asLiteral(std::string_view bytes)
        {
            constexpr std::string_view kHex = "0123456789abcdef";
            std::string text = "\"";
            for (const char c : bytes)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (c == '"' || c == '\\')
                    text += std::string("\\") + c;
                else if (byte >= 0x20 && byte < 0x7f)
                    text += c;
                else
                    text += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 15];
            }
            return text + "\"";
        }
    } // namespace

    // A string literal as a value: its bytes' constant (Strings.cpp) and their count, a constant itself, so that a
    // constant of the top level may be one.
    Lowering::Value Lowering::stringValue(const ast::StringExpr& literal)
    {
        auto* type = llvm::cast<llvm::StructType>(typeOf(ast::Type::String));
        llvm::Constant* bytes = llvm::cast<llvm::Constant>(stringConstant(literal));
        llvm::Constant* size = builder_.getInt32(static_cast<std::uint32_t>(literal.value.size()));
        return {llvm::ConstantStruct::get(type, {bytes, size}), ast::Type::String};
    }

    // The string at `address`: its pointer, loaded as a pointer, and its length, as an int.
    llvm::Value* Lowering::loadString(llvm::Value* address, const std::string& name)
    {
        llvm::Type* type = typeOf(ast::Type::String);
        const std::string stem = name.empty() ? "string" : name;
        llvm::LoadInst* data =
            builder_.CreateLoad(builder_.getPtrTy(), builder_.CreateStructGEP(type, address, 0), stem + ".data");
        tag(data, ast::Type::pointerTo(ast::Kind::Char, true));
        llvm::LoadInst* size =
            builder_.CreateLoad(builder_.getInt32Ty(), builder_.CreateStructGEP(type, address, 1), stem + ".size");
        tag(size, ast::Type::Int);
        llvm::Value* value = builder_.CreateInsertValue(llvm::PoisonValue::get(type), data, 0);
        return builder_.CreateInsertValue(value, size, 1, stem);
    }

    // `value`, a string, stored at `address` as its two members.
    void Lowering::storeString(llvm::Value* value, llvm::Value* address)
    {
        llvm::Type* type = typeOf(ast::Type::String);
        tag(builder_.CreateStore(builder_.CreateExtractValue(value, 0), builder_.CreateStructGEP(type, address, 0)),
            ast::Type::pointerTo(ast::Kind::Char, true));
        tag(builder_.CreateStore(builder_.CreateExtractValue(value, 1), builder_.CreateStructGEP(type, address, 1)),
            ast::Type::Int);
    }

    // `value` of `type` stored at `address`, with its tag: a string as its two members.
    void Lowering::store(llvm::Value* value, llvm::Value* address, ast::Type type)
    {
        if (type == ast::Type::String)
            storeString(value, address);
        else
            tag(builder_.CreateStore(value, address), type);
    }

    // Whether two strings hold the same bytes: the same length, and then the same bytes (the runtime's, which reads no
    // byte when the lengths differ). An i1.
    llvm::Value* Lowering::stringsEqual(llvm::Value* a, llvm::Value* b)
    {
        llvm::CallInst* equal = callRuntime("fminus_rt_string_equal", builder_.getInt32Ty(),
                                            {builder_.CreateExtractValue(a, 0), builder_.CreateExtractValue(a, 1),
                                             builder_.CreateExtractValue(b, 0), builder_.CreateExtractValue(b, 1)});
        equal->setName("equal");
        return builder_.CreateICmpNE(equal, builder_.getInt32(0), "streq");
    }

    // A binary operator with a string operand: == and != of two strings compare them by content; anything else is
    // clang's error for operands of a struct type (no <, no +, no &, and no string compares with a number or a
    // pointer).
    Lowering::Value Lowering::stringOperands(SourceLocation location, ast::BinaryOp op, Value lhs, Value rhs)
    {
        const bool equality = op == ast::BinaryOp::Equal || op == ast::BinaryOp::NotEqual;
        if (equality && lhs.type == ast::Type::String && rhs.type == ast::Type::String)
        {
            llvm::Value* equal = stringsEqual(lhs.ir, rhs.ir);
            return {op == ast::BinaryOp::Equal ? equal : builder_.CreateNot(equal, "strne"), ast::Type::Bool};
        }
        error(location, "invalid operands to binary expression (" + quoted(ast::typeName(lhs.type)) + " and "
                            + quoted(ast::typeName(rhs.type)) + ")");
        return poison(isComparison(op) ? ast::Type::Bool : isBitwise(op) ? ast::Type::Int : ast::Type::String);
    }

    // `a == "lit"`, `"lit" != a`: a literal an operand of a binary operator beside a string is one, which == and !=
    // compare by content and every other operator refuses, as it refuses two strings. The other operand is lowered
    // first (a literal computes nothing, so the order changes nothing the program does), and the literal is a string
    // when that is one, or when it failed (its own error says why: the literal gives no second); otherwise it is what
    // it was, C's char array, and the error a literal is there. Two literals are two strings.
    Lowering::Value Lowering::lowerLiteralOperand(SourceLocation location, const ast::BinaryExpr& operation)
    {
        const bool lhsLiteral = isString(*operation.lhs);
        if (lhsLiteral && isString(*operation.rhs))
            return binary(location, operation.op, lowerAs(*operation.lhs, ast::Type::String),
                          lowerAs(*operation.rhs, ast::Type::String));
        const ast::Expr& literal = lhsLiteral ? *operation.lhs : *operation.rhs;
        const Value other = lower(lhsLiteral ? *operation.rhs : *operation.lhs);
        const Value text =
            other.type == ast::Type::String || other.failed ? lowerAs(literal, ast::Type::String) : lower(literal);
        return lhsLiteral ? binary(location, operation.op, text, other) : binary(location, operation.op, other, text);
    }

    // A string_* builtin made inline (Calls.cpp's table, its symbol empty), from its arguments as its parameters took
    // them: each string a {ptr, i32}, and the rest converted. string_byte's byte is read only inside the string, behind
    // a branch, and untagged: a string may view bytes of any type.
    Lowering::Value Lowering::lowerText(SourceLocation location, const RuntimeBuiltin& builtin,
                                        const std::vector<llvm::Value*>& args)
    {
        const std::string_view name = builtin.name;
        const auto data = [&](llvm::Value* text) { return builder_.CreateExtractValue(text, 0, "data"); };
        const auto size = [&](llvm::Value* text) { return builder_.CreateExtractValue(text, 1, "size"); };
        auto* type = llvm::cast<llvm::StructType>(typeOf(ast::Type::String));
        const auto make = [&](llvm::Value* bytes, llvm::Value* count, const char* named) {
            llvm::Value* text = builder_.CreateInsertValue(llvm::PoisonValue::get(type), bytes, 0);
            return Value{builder_.CreateInsertValue(text, count, 1, named), ast::Type::String};
        };
        // The bytes from `from` (an i32 within the string) on: a plain getelementptr, which an empty string's null
        // takes with 0.
        const auto at = [&](llvm::Value* bytes, llvm::Value* from) {
            return builder_.CreateGEP(builder_.getInt8Ty(), bytes, builder_.CreateSExt(from, builder_.getInt64Ty()),
                                      "at");
        };
        const auto equalBytes = [&](llvm::Value* a, llvm::Value* an, llvm::Value* b, llvm::Value* bn) {
            llvm::CallInst* equal = callRuntime("fminus_rt_string_equal", builder_.getInt32Ty(), {a, an, b, bn});
            equal->setName("equal");
            return builder_.CreateICmpNE(equal, builder_.getInt32(0), std::string(name));
        };
        if (name == "string_size")
            return {size(args[0]), ast::Type::Int};
        if (name == "string_data")
            return {data(args[0]), resultType(builtin.result)};
        if (name == "string_byte")
        {
            llvm::Value* index = args[1];
            llvm::Value* inside = builder_.CreateICmpULT(index, size(args[0]), "inside");   // a negative one is not
            llvm::Value* bytes = data(args[0]);
            llvm::BasicBlock* from = builder_.GetInsertBlock();
            llvm::BasicBlock* read = newBlock("byte.read");
            llvm::BasicBlock* end = newBlock("byte.end");
            builder_.CreateCondBr(inside, read, end);
            builder_.SetInsertPoint(read);
            llvm::Value* byte = builder_.CreateZExt(builder_.CreateLoad(builder_.getInt8Ty(), at(bytes, index), "byte"),
                                                    builder_.getInt32Ty(), "conv");
            builder_.CreateBr(end);
            builder_.SetInsertPoint(end);
            llvm::PHINode* result = builder_.CreatePHI(builder_.getInt32Ty(), 2, "string_byte");
            result->addIncoming(builder_.getInt32(static_cast<std::uint32_t>(-1)), from);
            result->addIncoming(byte, read);
            return {result, ast::Type::Int};
        }
        if (name == "string_slice")
        {
            // from clamped to [0, size], to clamped to [from, size]: crossing ends are the empty string at `from`.
            llvm::Value* count = size(args[0]);
            llvm::Value* zero = builder_.getInt32(0);
            llvm::Value* first = builder_.CreateBinaryIntrinsic(
                llvm::Intrinsic::smax, zero, builder_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, args[1], count),
                nullptr, "from");
            llvm::Value* last = builder_.CreateBinaryIntrinsic(
                llvm::Intrinsic::smax, first, builder_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, args[2], count),
                nullptr, "to");
            return make(at(data(args[0]), first), builder_.CreateNSWSub(last, first, "count"), "string_slice");
        }
        if (name == "string_starts" || name == "string_ends")
        {
            // The string's first (or last) as many bytes as `with` has, compared with it; when the string is shorter,
            // the whole string, whose length differs, so nothing is read.
            llvm::Value* count = size(args[0]);
            llvm::Value* wanted = size(args[1]);
            llvm::Value* fits = builder_.CreateICmpSLE(wanted, count, "fits");
            llvm::Value* length = builder_.CreateSelect(fits, wanted, count, "length");
            llvm::Value* bytes = data(args[0]);
            if (name == "string_ends")
                bytes = at(bytes, builder_.CreateSelect(fits, builder_.CreateNSWSub(count, wanted),
                                                        builder_.getInt32(0), "skip"));
            return {equalBytes(bytes, length, data(args[1]), wanted), ast::Type::Bool};
        }
        // string_of(data, size): the view, or the empty string for a null `data` or a negative size.
        llvm::Value* empty =
            builder_.CreateOr(builder_.CreateIsNull(args[0], "null"),
                              builder_.CreateICmpSLT(args[1], builder_.getInt32(0), "negative"), "empty");
        (void)location;
        return make(builder_.CreateSelect(empty, llvm::ConstantPointerNull::get(builder_.getPtrTy()), args[0]),
                    builder_.CreateSelect(empty, builder_.getInt32(0), args[1]), "string_of");
    }

    // A case label of a switch on a string: its value a string literal (adjacent ones one), whose bytes no case before
    // it has (clang's error for a duplicate, with its note), and the block the switch jumps to for it, zeroing what
    // the jump passes as an integer case's does (bypass). Anything else is an error, and the switch's comparisons are
    // made without it.
    void Lowering::textCase(Switch& branch, const ast::Expr& value, llvm::BasicBlock* block)
    {
        if (!isString(value))
        {
            error(begin(value), "case value of a switch on a 'string' must be a string literal");
            branch.erroneous = true;
            return;
        }
        const auto& literal = std::get<ast::StringExpr>(value.node);
        const CaseLabel label{value.location, ""};
        const auto [previous, added] = branch.textCases.try_emplace(literal.value, label);
        if (added)
        {
            branch.texts.emplace_back(literal.value, bypass(branch, block));
            return;
        }
        branch.erroneous = true;
        diagnostics_.error(label.location, "duplicate case value " + quoted(asLiteral(literal.value)),
                           {Diagnostic{previous->second.location, "previous case defined here", {}}});
        previous->second = label;
    }

    // A switch on a string, its body lowered: in place of the placeholder that ended its value's block, a comparison
    // with each case's literal in the labels' order (sw.next between them), jumping to the first that is equal, and
    // then to the default's block, or past the switch (`end`).
    void Lowering::compareTexts(Switch& branch, llvm::BasicBlock* end)
    {
        const llvm::IRBuilderBase::InsertPointGuard resume(builder_);
        llvm::BasicBlock* block = branch.compare->getParent();
        branch.compare->eraseFromParent();
        builder_.SetInsertPoint(block);
        for (const auto& [bytes, target] : branch.texts)
        {
            const ast::StringExpr literal{bytes, {}};
            llvm::Value* equal = stringsEqual(branch.text, stringValue(literal).ir);
            llvm::BasicBlock* next = newBlock("sw.next");
            builder_.CreateCondBr(equal, target, next);
            builder_.SetInsertPoint(next);
        }
        builder_.CreateBr(branch.otherwise != nullptr ? branch.otherwise : end);
    }

    // `char name[8] = "abc";`, C's: the literal's bytes at the start of the array, which starts at 0 (so the rest stay
    // 0), or clang's error, at the literal, when they do not fit. C counts the NUL it puts after them only where it
    // fits (`char name[3] = "abc";` holds three bytes), and so does FMinus, whose strings have none.
    void Lowering::initializeChars(SourceLocation location, llvm::Value* address, ast::Type type,
                                   const ast::StringExpr& literal)
    {
        const std::uint64_t bytes = literal.value.size();
        if (bytes > static_cast<std::uint64_t>(type.count))
        {
            error(location, "initializer-string for char array is too long, array size is " + std::to_string(type.count)
                                + " but initializer has size " + std::to_string(bytes + 1)
                                + " (including the null terminating character)");
            return;
        }
        if (bytes > 0)
            builder_.CreateMemCpy(address, llvm::Align(1), stringConstant(literal), llvm::Align(1), bytes);
    }
} // namespace fminus::ir::detail
