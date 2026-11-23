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

// Portable external symbol ABI contract shared between Phase 1 interpreter (libffi)
// and Phase 2 LLVM AOT (declare external + native C ABI call).
struct FfiBinding {
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr = 0;
    uint64_t ret_shape_addr = 0;
    SymbolId cc = 0;           // Calling convention symbol (e.g. "c")
};

// Signedness is encoded directly into LLVM-aligned opcodes — zero redundant fields across all 5 variants.
enum class IntBinKind : uint8_t {
    Add, Sub, Mul, SDiv, UDiv, SRem, URem, BitAnd, BitOr, BitXor, Shl, AShr, LShr,
};

enum class IntUnaryKind : uint8_t {
    BitNot,
};

enum class FloatBinKind : uint8_t {
    Add, Sub, Mul, Div,
};

enum class CastKind : uint8_t {
    SExt, ZExt, Trunc, FPExt, FPTrunc, SIToFP, UIToFP, FPToSI, FPToUI,
};

enum class CmpKind : uint8_t {
    EqBytes, SLt, ULt, SLe, ULe, FEq, FLt, FLe,
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

// Pure-data hardware / LLVM IR intrinsic specification (zero std::function closure).
using IntrinsicSpec = std::variant<IntBinOp, IntUnaryOp, FloatBinOp, CastOp, CmpOp>;

struct AstProto {
    Ast::ExprPtr ast;
};

struct NativeProto {
    std::string extern_name;
    NativeFn fn;
};

struct FfiProto {
    FfiBinding binding;
    NativeFn fn;
};

struct IntrinsicProto {
    IntrinsicSpec spec;
};

// Pure-data closed sum type residing in PrototypeArena (.rodata).
// Exposed in Lang as an 8-byte opaque handle of shape `prototype_shape`.
// All consumers (Phase 1 Evaluator, Phase 2 LLVM codegen, serializer) dispatch externally via std::visit.
struct PrototypeObject {
    std::variant<AstProto, NativeProto, FfiProto, IntrinsicProto> payload;
};

class PrototypeArena {
private:
    std::vector<std::unique_ptr<PrototypeObject>> objects_;
    std::unordered_map<std::string, uint64_t> native_by_name_;

public:
    uint64_t create_ast(Ast::ExprPtr expr);
    uint64_t create_native(std::string extern_name, NativeFn fn);
    uint64_t create_ffi(FfiBinding binding, NativeFn fn);
    uint64_t create_intrinsic(IntrinsicSpec spec);
    const PrototypeObject* get(uint64_t addr) const;
    uint64_t find_native(const std::string& extern_name) const noexcept;
    size_t count() const noexcept { return objects_.size(); }
};

} // namespace reverSilly::Lang::Runtime
