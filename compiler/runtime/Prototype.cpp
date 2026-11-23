#include "Prototype.h"

#include <stdexcept>
#include <utility>

namespace reverSilly::Lang::Runtime {

uint64_t PrototypeArena::create_ast(Ast::ExprPtr expr) {
    auto obj = std::make_unique<PrototypeObject>(PrototypeObject{AstProto{std::move(expr)}});
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_native(std::string extern_name, NativeFn fn) {
    std::string key = extern_name;
    auto obj = std::make_unique<PrototypeObject>(
        PrototypeObject{NativeProto{std::move(extern_name), std::move(fn)}});
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    native_by_name_[std::move(key)] = addr;
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_ffi_extern(FfiBinding binding) {
    auto obj = std::make_unique<PrototypeObject>(PrototypeObject{FfiProto{std::move(binding)}});
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_ffi_bind_ptr(FfiBindPtrSpec spec) {
    auto obj = std::make_unique<PrototypeObject>(PrototypeObject{FfiBindPtrProto{spec}});
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_bound_ffi(BoundFfiSpec spec) {
    auto obj = std::make_unique<PrototypeObject>(PrototypeObject{BoundFfiProto{spec}});
    uint64_t addr = reinterpret_cast<uint64_t>(obj.get());
    objects_.push_back(std::move(obj));
    return addr;
}

uint64_t PrototypeArena::create_intrinsic(IntrinsicSpec spec) {
    auto obj = std::make_unique<PrototypeObject>(PrototypeObject{IntrinsicProto{spec}});
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
