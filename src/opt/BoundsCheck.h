// src/opt/BoundsCheck.h: bounds checks, for development builds. An index past an array's end, or before its start,
// reads or writes whatever lies beside the array: another field of the module, the stack, a host's memory. In a plugin
// that corrupts the host or crashes it, perhaps much later. Compiled with the checks (ir::Options::boundsCheck, which
// ir::lower records as the module flag below), every access whose array the compiler can see is checked against its
// extent: a[i], m.taps[i], p->taps[i], voices[v] and *(a + i), a pointer formed from one (a + i, &a[i], which may
// point one past its end, as C allows), and a poly_load or poly_store through one (each lane it reaches; a masked one,
// each lane its mask takes). A pointer the program keeps (in a variable, a field or a parameter, what a function
// returns, a heap block, a host's buffer) has no extent the compiler knows, and its accesses are not checked.
//
// The code generator (ir/Bounds.cpp) puts each check where the access is, before the optimiser runs, as clang's
// -fsanitize=array-bounds does with -fsanitize-trap: an unsigned comparison of the index against the extent, and on
// the unlikely side a call of kBoundsStopFunction, which never returns, with the check's number and the index. The
// optimiser sees a trap: it removes a check it can prove (a loop over the array that stays within it), keeps the
// others, and moves nothing past one. Its description of the check (BoundsSite, below) goes into the module's named
// metadata, kBoundsSitesMetadata, in order: check n is its nth operand.
//
// This pass runs at the end of the pipeline, before the loop budget's (opt/LoopBudget.h), at every level, and makes
// each trap a return. A failed check records the check's number and the index in the program's record, the symbol
// fminus_rt_bounds, a { i32 site; i32 reserved; i64 index } (runtime/Runtime.h), unless the record holds a check
// already: it keeps the first. Then the function returns at once, 0, null or nothing, and each of the program's
// functions that called it, seeing the record set after the call, returns at once too, back to the host: no caller
// goes on with a value that was never computed. The host reads the record after the call and clears it before the next
// (Program::boundsViolation and clearBoundsViolation, include/fminus/FMinus.h). While it holds a check, a call returns
// at its first call of a function that has checks, however it got there. Each access to the record is a relaxed
// (monotonic) atomic, the site's a compare-and-swap from 0, so a parallel region's threads may share it: the first to
// fail is recorded, and the others' parts stop at their next call of a function that has checks.
//
// What the call leaves in memory is whichever of the optimised code's stores had run, as with the loop budget: a
// module's field kept in a register through a loop is stored after it, so the instance may be in a state the source
// never passes through. A host re-initialises it, or discards it, before running it again.
//
// Off, the code is as it was: the code generator emits no check, defines no record and adds no metadata, and the pass
// leaves a module without the flag alone.
#pragma once

#include <llvm/IR/PassManager.h>

#include <cstdint>
#include <string>
#include <vector>

namespace llvm
{
    class Function;
    class GlobalVariable;
    class Module;
} // namespace llvm

namespace fminus::opt
{
    // The module flag that turns the pass on: 1 (ir::lower writes it when ir::Options::boundsCheck is set). A module
    // without it, or with 0, is left as it is.
    inline constexpr const char* kBoundsCheckFlag = "fminus.bounds-check";

    // The record's symbol, which the generated code writes and its host reads (runtime/Runtime.h).
    inline constexpr const char* kBoundsSymbol = "fminus_rt_bounds";

    // What the code generator calls where a check fails, until the pass makes it a return: void (i32 site, i64 index),
    // noreturn, nounwind, cold, and writing only memory the program cannot reach (the record). No function of the
    // program can have its name (FMinus's names have no '.').
    inline constexpr const char* kBoundsStopFunction = "fminus.bounds.stop";

    // The named metadata that describes each check, in the order of their numbers, from 1.
    inline constexpr const char* kBoundsSitesMetadata = "fminus.bounds.sites";

    // The string attribute of a function the pass changed, so that IR optimised once already (fminus-build at -O3,
    // say) is not changed twice: such a function may return early.
    inline constexpr const char* kBoundsCheckAttribute = "fminus-bounds-check";

    // A check, as the source has it: where the access is, the array, and what the access reaches of it.
    struct BoundsSite
    {
        std::string file;          // the source file, as its errors name it (the program's own, or an #include's)
        int line = 0;
        int column = 0;
        std::string array;         // the array as the source names it ("buf", "m.taps", "voices[v].line"), or empty
        std::string type;          // its type, as FMinus writes it: "float[16]"
        std::int64_t extent = 0;   // its elements
        // How many elements from the index the access reaches: 1 for an element (a[i], *p, p->y), a poly_load's or
        // poly_store's lanes, and 0 for a pointer formed (a + i, &a[i]), which may be one past the end.
        int width = 1;
    };

    // The record in `module`, the global { i32 site; i32 reserved; i64 index } of kBoundsSymbol, defined there with no
    // check in it ({0, 0, 0}), and made if it is not there yet.
    llvm::GlobalVariable* boundsGlobal(llvm::Module& module);

    // kBoundsStopFunction in `module`, declared with its attributes if it is not there yet.
    llvm::Function* boundsStopFunction(llvm::Module& module);

    // Adds `site` to the module's checks, and returns its number, from 1.
    std::int32_t addBoundsSite(llvm::Module& module, const BoundsSite& site);

    // The module's checks, in the order of their numbers: check n is element n - 1.
    std::vector<BoundsSite> boundsSites(const llvm::Module& module);

    // What a failed check says, as a message of its own, in UBSan's words with the array's name and where it is:
    //   echo.fm:12:9: index 16 out of bounds for 'line' of type 'float[16]'
    //   echo.fm:14:20: pointer offset 17 out of bounds for 'line' of type 'float[16]'
    //   echo.fm:20:13: 4 elements from index 14 out of bounds for 'line' of type 'float[16]'
    // (an array with no name is "for type 'float[16]'"). Also as what comes before the index and what after it, for
    // an executable's message, which its C main prints (support/Aot.cpp).
    std::string boundsMessage(const BoundsSite& site, std::int64_t index);
    std::string boundsMessageBefore(const BoundsSite& site);
    std::string boundsMessageAfter(const BoundsSite& site);

    // Makes every failed check of a module that has the flag a return that records it, and every call of a function
    // that may stop one that returns too when it did (BoundsCheck.cpp says how), at every level.
    struct BoundsCheckPass : llvm::PassInfoMixin<BoundsCheckPass>
    {
        llvm::PreservedAnalyses run(llvm::Module& module, llvm::ModuleAnalysisManager& analyses);

        static bool isRequired()
        {
            return true;   // at -O0 too, and in a function marked optnone
        }
    };
} // namespace fminus::opt
