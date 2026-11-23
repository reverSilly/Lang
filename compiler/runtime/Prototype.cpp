#include "Prototype.h"

#include <stdexcept>
#include <utility>

namespace reverSilly::Lang::Runtime {

uint64_t PrototypeArena::create_ast(Ast::ExprPtr expr) {
    auto obj = std::make_unique<PrototypeObject>();
    obj->kind = CodeKind::Ast;
    obj->ast = std::move(expr);
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_native(std::string extern_name, NativeFn fn) {
    auto obj = std::make_unique<PrototypeObject>();
    obj->kind = CodeKind::NativeSym;
    obj->extern_name = std::move(extern_name);
    obj->native_fn = std::move(fn);
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    native_by_name_[obj->extern_name] = addr;
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_ffi(FfiBinding binding, NativeFn fn) {
    auto obj = std::make_unique<PrototypeObject>();
    obj->kind = CodeKind::NativeSym;
    obj->extern_name = binding.symbol_name;
    obj->ffi_binding = std::move(binding);
    obj->native_fn = std::move(fn);
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

const PrototypeObject* PrototypeArena::get(uint64_t addr) const {
    if (addr == 0) {
        throw std::runtime_error("PrototypeArena::get: null prototype handle");
    }
    return reinterpret_cast<const PrototypeObject*>(addr);
}

uint64_t PrototypeArena::find_native(const std::string& extern_name) const noexcept {
    auto it = native_by_name_.find(extern_name);
    return it == native_by_name_.end() ? 0 : it->second;
}

} // namespace reverSilly::Lang::Runtime
