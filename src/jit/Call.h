// src/jit/Call.h: calling a JIT'd function whose signature is known only as the host runs (Program::call,
// include/fminus/FMinus.h; fminus --run). C++ calls a function pointer only through a type it was compiled with, so for
// each such function FMinus compiles a thunk, a function of one C++ type, void(const CallSlot* arguments, CallSlot*
// result): it loads each argument from its slot as the function's parameter type, calls the function as C calls it, and
// stores its result. The thunk is LLVM IR in a module of its own, added to the program's JIT beside the program, so the
// target's calling convention is LLVM's to get right on every target (arm64's, Apple's, x86-64's System V and Windows
// conventions): which register each argument goes in, the ones that go on the stack, a bool's extension. The program's
// own module is as it was, and a host that never calls by name compiles no thunk.
#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace fminus
{
    namespace ast
    {
        struct Type;
    } // namespace ast

    namespace jit
    {
        class Jit;

        // The types a thunk passes: C's scalars, and pointers.
        enum class CallType : std::uint8_t
        {
            Void,   // a result only: none
            Bool,
            Int,
            Float,
            Double,
            Pointer,
        };

        // An argument or a result in a thunk's memory: 8 bytes, aligned to 8, the value at its start in its own type:
        // a bool a byte, 0 or 1 (any other byte reads as true), an int 4 bytes, a float 4, a double 8, a pointer 8.
        struct alignas(8) CallSlot
        {
            unsigned char bytes[8] = {};

            template <typename T>
            static CallSlot of(T value)
            {
                static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 8);
                CallSlot slot;
                std::memcpy(slot.bytes, &value, sizeof(T));
                return slot;
            }

            template <typename T>
            T as() const
            {
                static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 8);
                T value;
                std::memcpy(&value, bytes, sizeof(T));
                return value;
            }
        };

        using Thunk = void (*)(const CallSlot* arguments, CallSlot* result);

        // The thunk's symbol for `function`: "fminus.call." and its name, which no FMinus name can be.
        std::string thunkName(llvm::StringRef function);

        // The thunk of `function`, which returns `result` and takes `parameters`, compiled into `jit` as a module of
        // its own, on this thread (a module of one small function: about a millisecond). `function` must be one the
        // JIT has: look it up first, so that a name it lacks is that lookup's error. One thunk a function a JIT: a
        // second for the same function is LLVM's error, a duplicate definition.
        llvm::Expected<Thunk> callThunk(Jit& jit, llvm::StringRef function, CallType result,
                                        llvm::ArrayRef<CallType> parameters);

        // How a thunk passes a value of FMinus's `type`: a scalar as itself (an enumerated type's as its int or bool),
        // a pointer as a pointer, void as no result. Nothing for any other type (a vector, a module or an array, which
        // FMinus passes by pointer).
        std::optional<CallType> callType(const ast::Type& type);
    } // namespace jit
} // namespace fminus
