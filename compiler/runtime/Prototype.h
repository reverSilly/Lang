#pragma once

#include "../ast/Ast.h"
#include "Shape.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Eval {
class Evaluator;
}

namespace reverSilly::Lang::Runtime {

enum class CodeKind : uint8_t {
    Ast = 1,
    NativeSym = 2,
};

using NativeFn = std::function<Value(Eval::Evaluator&)>;

// Live, linked code object residing in PrototypeArena (.rodata).
// Exposed in Lang as an 8-byte opaque Handle of shape `prototype_shape`.
struct PrototypeObject {
    CodeKind kind = CodeKind::Ast;
    Ast::ExprPtr ast;
    std::string extern_name;
    NativeFn native_fn;
};

class PrototypeArena {
private:
    std::vector<std::unique_ptr<PrototypeObject>> objects_;
    std::unordered_map<std::string, uint64_t> native_by_name_;

public:
    uint64_t create_ast(Ast::ExprPtr expr);
    uint64_t create_native(std::string extern_name, NativeFn fn);
    const PrototypeObject* get(uint64_t addr) const;
    uint64_t find_native(const std::string& extern_name) const noexcept;
    size_t count() const noexcept { return objects_.size(); }
};

} // namespace reverSilly::Lang::Runtime
