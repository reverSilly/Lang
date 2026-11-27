#pragma once

#include "../ast/Ast.h"
#include "Shape.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace reverSilly::Lang::Eval {
class Evaluator;
}

namespace reverSilly::Lang::Runtime {

using NativeFn = std::function<Value(Eval::Evaluator&)>;

using Ast::IntBinKind;
using Ast::IntUnaryKind;
using Ast::FloatBinKind;
using Ast::CastKind;
using Ast::CmpKind;
using Ast::RuntimeIntrinsicKind;

// Portable declarative external C symbol specification (zero dlopen handle, zero NativeFn).
struct FfiBinding {
    std::string lib_name;      // "" = default process/libc, "m" = libm, ...
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr = 0;
    uint64_t ret_shape_addr = 0;
    SymbolId cc = 0;           // Calling convention symbol (e.g. "c")
};

// Two-stage indirect C function pointer binder (compile-time ABI, runtime fn_ptr slot).
struct FfiBindPtrSpec {
    SymbolId fn_slot = 0;
    uint64_t args_shape_addr = 0;
    uint64_t ret_shape_addr = 0;
    SymbolId cc = 0;
};

// Runtime-bound indirect C function pointer (Stage 2 output of FfiBindPtrProto; non-stageable).
struct BoundFfiSpec {
    void* fn_ptr = nullptr;
    uint64_t args_shape_addr = 0;
    uint64_t ret_shape_addr = 0;
    SymbolId cc = 0;
};

struct IntBinOp {
    IntBinKind op;
    uint8_t width = 0;
    SymbolId lhs = 0;
    SymbolId rhs = 0;
};

struct IntUnaryOp {
    IntUnaryKind op;
    uint8_t width = 0;
    SymbolId src = 0;
};

struct FloatBinOp {
    FloatBinKind op;
    uint8_t width = 0;
    SymbolId lhs = 0;
    SymbolId rhs = 0;
};

struct CastOp {
    CastKind op;
    uint8_t from_width = 0;
    uint8_t to_width = 0;
    SymbolId src = 0;
};

struct CmpOp {
    CmpKind op;
    uint8_t width = 0;
    SymbolId lhs = 0;
    SymbolId rhs = 0;
};

struct RuntimeOp {
    RuntimeIntrinsicKind op;
};

// Pure-data hardware / LLVM IR / runtime reflection intrinsic specification (zero std::function closure).
using IntrinsicSpec = std::variant<IntBinOp, IntUnaryOp, FloatBinOp, CastOp, CmpOp, RuntimeOp>;

struct AstProto {
    Ast::ExprPtr ast;
};

struct NativeProto {
    std::string extern_name;
    NativeFn fn;
};

struct FfiProto {
    FfiBinding binding;
};

struct FfiBindPtrProto {
    FfiBindPtrSpec spec;
};

struct BoundFfiProto {
    BoundFfiSpec spec;
};

struct IntrinsicProto {
    IntrinsicSpec spec;
};

// Pure-data closed sum type residing in PrototypeArena.
// Exposed in Lang as an 8-byte opaque handle of shape `prototype_shape`.
// All consumers (Phase 1 Evaluator, Phase 2 LLVM codegen, AST reifier) dispatch externally via std::visit.
struct PrototypeObject {
    std::variant<
        AstProto,
        NativeProto,
        FfiProto,
        FfiBindPtrProto,
        BoundFfiProto,
        IntrinsicProto> payload;
};

class PrototypeArena {
private:
    std::vector<std::unique_ptr<PrototypeObject>> objects_;
    std::unordered_map<std::string, uint64_t> native_by_name_;

public:
    uint64_t create_ast(Ast::ExprPtr expr);
    uint64_t create_native(std::string extern_name, NativeFn fn);
    uint64_t create_ffi_extern(FfiBinding binding);
    uint64_t create_ffi_bind_ptr(FfiBindPtrSpec spec);
    uint64_t create_bound_ffi(BoundFfiSpec spec);
    uint64_t create_intrinsic(IntrinsicSpec spec);
    const PrototypeObject* get(uint64_t addr) const;
    uint64_t find_native(const std::string& extern_name) const noexcept;
    size_t count() const noexcept { return objects_.size(); }
};

} // namespace reverSilly::Lang::Runtime
