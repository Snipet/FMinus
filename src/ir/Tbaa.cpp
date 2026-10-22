// src/ir/Tbaa.cpp: C's strict-aliasing rule, told to LLVM as type-based alias analysis (TBAA). FMinus keeps C's
// effective-type rule (C11 6.5p6-7): an object is read and written only through an lvalue of its own type, so a store
// of a float cannot change an int or a pointer. LLVM learns it from a tag on each load and store, `!tbaa`, that names
// the access's type in a tree of types: two accesses whose types are different leaves of one tree do not alias. Then
// a method's loop over its module's buffer (`tail[i] += result[i]`, both fields) loads the fields once, before the
// loop, where it would load them again after every store of a float, and the loop runs on vectors.
//
// FMinus's tree is a root and four scalar types under it, in clang's style (MDBuilder::createTBAAScalarTypeNode) and
// with clang's names:
//
//     !{!"FMinus TBAA"}                   the root (accesses tagged in another tree, clang's, may alias FMinus's)
//       !{!"float", root, i64 0}          float, and every float vector: float2, float4, float8, poly float
//       !{!"int", root, i64 0}
//       !{!"bool", root, i64 0}           bool, and the lane masks
//       !{!"any pointer", root, i64 0}    every pointer, whatever it points to, void* too
//
// A float vector is its elements' type, so a float4* over a float buffer, poly_load and poly_store, and the FFT's
// float2 bins reach float memory and back, as they must. One pointer type is enough: FMinus has no pointer to a
// pointer, so a pointer in memory is only ever reached as itself. Each tag is a scalar access tag, its base type its
// access type, at offset 0; struct-path tags, which would also tell one float field from another, are not emitted.
//
// What is tagged: every load and store of a value: read() and write() (Memory.cpp: variables, elements and fields),
// an array's initialiser list, poly_load and poly_store (Calls.cpp; LLVM reads a masked load's or store's tag as it
// reads a load's), and a method's reads of its parameters and of `this` (Modules.cpp): a knob's value as its type, a
// stream's pointer as a pointer, its samples as floats. What is not: a module or an array is never loaded or stored
// whole, and its copy (a memcpy) and a declaration's zeroing (a memset) carry no tag, so they may touch any type, as a
// char access may in C; nor do the stores that start a parameter's or a declaration's stack slot (Functions.cpp,
// Statements.cpp), which mem2reg removes. An untagged access may alias anything, so leaving one out is always safe.
// With Options::strictAliasing off (clang's -fno-strict-aliasing) nothing is tagged.
#include "ir/Lowering.h"

#include <llvm/IR/MDBuilder.h>

namespace fminus::ir::detail
{
    namespace
    {
        // The name of the TBAA type an access of `kind` has, or nullptr for what is never loaded or stored whole.
        const char* tbaaTypeName(ast::Kind kind)
        {
            switch (kind)
            {
            case ast::Kind::Float:
            case ast::Kind::Float2:
            case ast::Kind::Float4:
            case ast::Kind::Float8:
            case ast::Kind::PolyFloat:
                return "float";
            case ast::Kind::Int:
                return "int";
            case ast::Kind::Bool:
            case ast::Kind::Bool2:
            case ast::Kind::Bool4:
            case ast::Kind::Bool8:
            case ast::Kind::PolyBool:
                return "bool";
            case ast::Kind::Pointer:
                return "any pointer";
            case ast::Kind::Void:
            case ast::Kind::Module:   // its fields are loaded and stored, each as its own type
            case ast::Kind::Array:    // likewise its elements
                return nullptr;
            }
            return nullptr;
        }
    } // namespace

    // The access tag of a load or a store of `type`, or nullptr for a type that is never loaded or stored whole. The
    // context uniques metadata, so asking twice gives the same node, and the tree is made once a module.
    llvm::MDNode* Lowering::accessTag(ast::Type type)
    {
        const char* name = tbaaTypeName(type.kind);
        if (name == nullptr)
            return nullptr;
        llvm::MDBuilder md(module_.getContext());
        llvm::MDNode* access = md.createTBAAScalarTypeNode(name, md.createTBAARoot("FMinus TBAA"));
        return md.createTBAAStructTagNode(access, access, 0);
    }

    // `access`, a load or a store of a value of `type` (or a masked one, a call), tagged with its type: what LLVM's
    // TypeBasedAA reads. Nothing when strict aliasing is off.
    void Lowering::tag(llvm::Instruction* access, ast::Type type)
    {
        if (!strictAliasing_)
            return;
        if (llvm::MDNode* node = accessTag(type))
            access->setMetadata(llvm::LLVMContext::MD_tbaa, node);
    }
} // namespace fminus::ir::detail
