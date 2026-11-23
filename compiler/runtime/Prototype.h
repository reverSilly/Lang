#pragma once

#include "../ast/Ast.h"
#include "Shape.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// Portable external symbol ABI contract shared between Phase 1 interpreter (libffi)
// and Phase 2 LLVM AOT (declare external + native C ABI call).
struct FfiBinding {
    std::string symbol_name;   // C symbol name
    uint64_t args_shape_addr = 0;
    uint64_t ret_shape_addr = 0;
    SymbolId cc = 0;           // Calling convention symbol (e.g. "c")
};

// Live, linked code object residing in PrototypeArena (.rodata).
// Exposed in Lang as an 8-byte opaque Handle of shape `prototype_shape`.
struct PrototypeObject {
    CodeKind kind = CodeKind::Ast;
    Ast::ExprPtr ast;
    std::string extern_name;
    NativeFn native_fn;
    std::optional<FfiBinding> ffi_binding;
};

class PrototypeArena {
private:
    std::vector<std::unique_ptr<PrototypeObject>> objects_;
    std::unordered_map<std::string, uint64_t> native_by_name_;

public:
    uint64_t create_ast(Ast::ExprPtr expr);
    uint64_t create_native(std::string extern_name, NativeFn fn);
    uint64_t create_ffi(FfiBinding binding, NativeFn fn);
    const PrototypeObject* get(uint64_t addr) const;
    uint64_t find_native(const std::string& extern_name) const noexcept;
    size_t count() const noexcept { return objects_.size(); }
};

} // namespace reverSilly::Lang::Runtime
