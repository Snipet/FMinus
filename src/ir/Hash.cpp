// src/ir/Hash.cpp: hash and randf, builtins of FMinus's own (C has none like them). hash(n) mixes an int, a counter or
// a seed, into a well-spread int; randf(n) is a float uniform in [-1, 1) from the same bits: white noise, a sample at a
// time, from a count of samples, with no state to keep (`out[i] = randf(start + i)`).
//
// Each is a few integer instructions inline, never a call: the same argument gives the same result, as a function of
// its argument alone (pure: LLVM may fold, hoist, merge or remove one, as it does an `add`). Every step is exact, an
// integer's and a float's that needs no rounding, so it gives the same bits in every lane of a vectorised loop and in
// its remainder, at any optimisation level, in every math mode, with subnormals flushed or not, and on any target; a
// loop of them vectorises into integer vector instructions (NEON's ushr, eor and mul.4s, and one scvtf.4s that takes
// the product by 2^-23 in: about three instructions a sample).
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>

namespace fminus::ir::detail
{
    bool isHash(std::string_view name)
    {
        return name == "hash" || name == "randf";
    }

    // The mixer: Chris Wellons's lowbias32, from his hash prospector (public domain), on the int's 32 bits as an
    // unsigned C would hold them: xor-shifts right (lshr: zeros shifted in) and multiplications that wrap (mul without
    // nsw, the overflow defined), as C computes on uint32_t:
    //     x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    // Each step is a bijection of the 32-bit values, so hash is one too: two ints never give the same hash. hash(0) is
    // 0.
    llvm::Value* Lowering::mixBits(llvm::Value* x)
    {
        const auto shiftXor = [&](llvm::Value* value, unsigned shift) {
            return builder_.CreateXor(value, builder_.CreateLShr(value, shift, "hash.shr"), "hash.xor");
        };
        x = shiftXor(x, 16);
        x = builder_.CreateMul(x, builder_.getInt32(0x7feb352du), "hash.mul");
        x = shiftXor(x, 15);
        x = builder_.CreateMul(x, builder_.getInt32(0x846ca68bu), "hash.mul");
        return shiftXor(x, 16);
    }

    // hash(n) and randf(n). The argument is an int, converted as it would be for a parameter of type int (a bool is 0
    // or 1, a float is truncated toward zero). randf takes the hash's top 24 bits as a signed int k, from -2^23 to
    // 2^23 - 1 (ashr: the sign shifted in), and gives k / 2^23: 2^24 values, evenly spaced 2^-23 apart from -1 to
    // 1 - 2^-23, each as likely, their mean -2^-24. Both steps are exact: k has 24 bits, which a float holds, and the
    // product by a power of two only moves the exponent (never into the subnormals).
    Lowering::Value Lowering::lowerHash(SourceLocation location, const ast::CallExpr& call,
                                        const std::vector<Value>& args)
    {
        if (args.size() != 1)
            return wrongArity(location, args.size(), 1);
        llvm::Value* hashed = mixBits(convert(args[0], ast::Type::Int, location, Use::Passing));
        if (call.callee == "hash")
            return {hashed, ast::Type::Int};
        llvm::Value* top = builder_.CreateAShr(hashed, 8, "randf.bits");
        llvm::Value* exact = builder_.CreateSIToFP(top, builder_.getFloatTy(), "randf.conv");
        return {builder_.CreateFMul(exact, llvm::ConstantFP::get(builder_.getFloatTy(), 0x1p-23), "randf"),
                ast::Type::Float};
    }
} // namespace fminus::ir::detail
