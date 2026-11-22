#pragma once

#include "../ast/Ast.h"
#include "Shape.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
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
    uint64_t create_ast(Ast::ExprPtr expr) {
        auto obj = std::make_unique<PrototypeObject>();
        obj->kind = CodeKind::Ast;
        obj->ast = std::move(expr);
        uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
        objects_.push_back(std::move(obj));
        return addr;
    }

    uint64_t create_native(std::string extern_name, NativeFn fn) {
        auto obj = std::make_unique<PrototypeObject>();
        obj->kind = CodeKind::NativeSym;
        obj->extern_name = std::move(extern_name);
        obj->native_fn = std::move(fn);
        uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
        native_by_name_[obj->extern_name] = addr;
        objects_.push_back(std::move(obj));
        return addr;
    }

    const PrototypeObject* get(uint64_t addr) const {
        if (addr == 0) {
            throw std::runtime_error("PrototypeArena::get: null prototype handle");
        }
        return reinterpret_cast<const PrototypeObject*>(addr);
    }

    uint64_t find_native(const std::string& extern_name) const noexcept {
        auto it = native_by_name_.find(extern_name);
        return it == native_by_name_.end() ? 0 : it->second;
    }

    size_t count() const noexcept { return objects_.size(); }
};

} // namespace reverSilly::Lang::Runtime
