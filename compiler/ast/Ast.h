#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace reverSilly::Lang::Ast {

enum class BuiltinKind : uint8_t {
    // 4 nullary environment & coroutine cursors
    This,
    Parent,
    Caller,
    In,

    // 5 evaluation & coroutine lifecycle / reflection primitives
    Evaluate,
    Block,
    New,
    Destroy,
    CoroRootFrame,

    // 5 allocation & slot state-machine primitives
    Allocate,
    AllocateVal,
    Touch,
    Bind,
    Drop,

    // 2 raw bit read/write primitives (POD/RAII-agnostic)
    Deref,
    Write,

    // 2 navigation & reflection primitives
    Field,
    Shape,

    // 3 control-flow & coroutine primitives
    Branch,
    Break,
    Resume,
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
    EqBytes, EqShape, SLt, ULt, SLe, ULe, FEq, FLt, FLe,
};

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

// Pure-value structural layout tree (zero RuntimeContext / ShapeArena dependency).
// Used only where a layout is needed without a value (ShapeVal, FFI ABI args/ret).
struct AstField;
struct AstShape {
    uint64_t size = 0;
    uint64_t align = 1;
    std::vector<AstField> fields;
};
struct AstField {
    std::string name;
    uint64_t offset = 0;
    AstShape shape;
};

// Environment-independent intrinsic specs (slot names stored as std::string for delayed interning).
struct AstIntBinOp   { IntBinKind op;   uint8_t width = 0; std::string lhs, rhs; };
struct AstIntUnaryOp { IntUnaryKind op; uint8_t width = 0; std::string src; };
struct AstFloatBinOp { FloatBinKind op; uint8_t width = 0; std::string lhs, rhs; };
struct AstCastOp     { CastKind op;     uint8_t from_width = 0, to_width = 0; std::string src; };
struct AstCmpOp      { CmpKind op;      uint8_t width = 0; std::string lhs, rhs; };

using AstIntrinsicSpec = std::variant<
    AstIntBinOp,
    AstIntUnaryOp,
    AstFloatBinOp,
    AstCastOp,
    AstCmpOp>;

// 8-variant standalone compile-time constant value (zero RuntimeContext dependency).
// Strictly excludes all Pointer, Handle, NativeProto, and runtime-bound BoundFfiProto values.
struct AstStructField;

struct RawBytes {
    uint64_t align = 1;
    std::vector<uint8_t> bytes;
};

struct SymbolVal {
    std::string name;
};

struct ShapeVal {
    AstShape shape;
};

struct AstProtoVal {
    ExprPtr ast;
};

struct IntrinsicVal {
    AstIntrinsicSpec spec;
};

struct FfiExternVal {
    std::string lib_name;
    std::string symbol_name;
    AstShape args;
    AstShape ret;
    std::string cc;
};

struct FfiBindPtrVal {
    std::string fn_slot;
    AstShape args;
    AstShape ret;
    std::string cc;
};

struct StructVal {
    uint64_t size = 0;
    uint64_t align = 1;
    std::vector<AstStructField> fields;
};

struct AstValue {
    using Variant = std::variant<
        RawBytes,
        SymbolVal,
        ShapeVal,
        AstProtoVal,
        IntrinsicVal,
        FfiExternVal,
        FfiBindPtrVal,
        StructVal>;

    Variant data;

    AstValue() : data(RawBytes{1, {}}) {}
    template <typename T>
    explicit AstValue(T&& v) : data(std::forward<T>(v)) {}
};

struct AstStructField {
    std::string name;
    uint64_t offset = 0;
    AstValue value;
};

// Layer 0 Word: delayed interning (resolved to 4-byte SymbolId in RuntimeContext).
struct WordExpr {
    std::string text;
};

// Layer 0 Builtin invocation: #name or #name(arg1, arg2, ...)
struct BuiltinExpr {
    BuiltinKind kind;
    std::vector<ExprPtr> args;
};

// Layer 0 sequence: a ; b (evaluates left, discards value, returns right)
struct SequenceExpr {
    std::vector<ExprPtr> items;
};

// Environment-independent constant value node.
struct ValueExpr {
    AstValue value;
};

struct Expr {
    using Variant = std::variant<
        WordExpr,
        BuiltinExpr,
        SequenceExpr,
        ValueExpr>;

    Variant node;

    template <typename T>
    explicit Expr(T&& n) : node(std::forward<T>(n)) {}
};

template <typename T, typename... Args>
inline ExprPtr make_expr(Args&&... args) {
    return std::make_shared<const Expr>(T{std::forward<Args>(args)...});
}

} // namespace reverSilly::Lang::Ast
