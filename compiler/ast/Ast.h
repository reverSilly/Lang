#pragma once

#include "../runtime/Shape.h"

#include <cstdint>
#include <memory>
#include <string>
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

    // 3 navigation, reflection & freeze primitives
    Field,
    Shape,
    Freeze,

    // 3 control-flow & coroutine primitives
    Branch,
    Break,
    Resume,
};

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

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

// Constant Value node (carries any embedded Value, including Shape and Prototype values)
struct ValueExpr {
    Runtime::Value value;
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
