#pragma once

#include "../runtime/Shape.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace reverSilly::Lang::Ast {

enum class BuiltinKind : uint8_t {
    // 5 nullary environment & slot cursors
    This,
    Parent,
    Unbound,
    Caller,
    In,

    // 4 evaluation & stage primitives
    Evaluate,
    Block,
    New,
    Compile,

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

// Layer 0 Word: automatically mapped to a 4-byte Symbol ID in Core.
struct WordExpr {
    std::string text;
};

// Layer 0 Builtin invocation: #name or #name(arg1, arg2, ...)
struct BuiltinExpr {
    BuiltinKind kind;
    std::vector<ExprPtr> args;
};

// Layer 0 unique lazy special form: ^Expr -> Prototype
struct PrototypeExpr {
    ExprPtr body;
};

// Layer 0 sequence: a ; b (evaluates left, discards value, returns right)
struct SequenceExpr {
    std::vector<ExprPtr> items;
};

// Layer 0 grouping: (Expr) or () for empty unit
struct GroupExpr {
    ExprPtr inner; // nullptr represents empty unit ()
};

// Reified constant Value node (produced by Layer 1 literal hooks or #compile folding)
struct ValueExpr {
    Runtime::Value value;
};

struct Expr {
    using Variant = std::variant<
        WordExpr,
        BuiltinExpr,
        PrototypeExpr,
        SequenceExpr,
        GroupExpr,
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
