#include "Capabilities.h"

#include <stdexcept>

namespace reverSilly::Lang::Eval {

uint64_t CapabilityHost::encode_handle(HandleTag tag, size_t idx) noexcept {
    return (static_cast<uint64_t>(tag) << 56) | static_cast<uint64_t>(idx);
}

size_t CapabilityHost::decode_handle(uint64_t handle, HandleTag expected, size_t pool_size, const char* what) {
    uint8_t tag = static_cast<uint8_t>(handle >> 56);
    size_t idx = static_cast<size_t>(handle & 0x00FFFFFFFFFFFFFFULL);
    if (tag != static_cast<uint8_t>(expected) || idx >= pool_size) {
        throw std::runtime_error(std::string("capabilities: invalid or mismatched ") + what + " handle");
    }
    return idx;
}

Runtime::Value CapabilityHost::read_field(Evaluator& ev, std::string_view name) {
    Runtime::ConstructingFrame* f = ev.rt.current_frame();
    Runtime::SymbolId sym = ev.rt.symbols.intern(name);
    Runtime::PointerLayout p = f->field(sym);
    if (p.is_null()) {
        throw std::runtime_error("capability missing required field '" + std::string(name) + "' in #this");
    }
    return ev.rt.deref(p);
}

std::span<const uint8_t> CapabilityHost::read_bytes_ptr(Evaluator& ev, std::string_view name) {
    Runtime::Value v = read_field(ev, name);
    if (v.shape_addr != ev.rt.shapes.pointer_shape) {
        throw std::runtime_error("capabilities: field '" + std::string(name) + "' must be a Pointer to raw bytes");
    }
    auto p = v.as_pod<Runtime::PointerLayout>();
    if (p.shape_ptr == 0) {
        throw std::runtime_error("capabilities: null shape_ptr on byte pointer '" + std::string(name) + "'");
    }
    const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(p.shape_ptr);
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.addr), sl->size);
}

std::vector<Runtime::Value> CapabilityHost::read_positional(Evaluator& ev) {
    Runtime::ConstructingFrame* f = ev.rt.current_frame();
    std::vector<Runtime::Value> out;
    for (size_t i = 0;; ++i) {
        Runtime::SymbolId sym = ev.rt.symbols.intern(std::to_string(i));
        Runtime::PointerLayout p = f->field(sym);
        if (p.is_null()) break;
        out.push_back(ev.rt.deref(p));
    }
    return out;
}

Runtime::Value CapabilityHost::store_bytes_ptr(Runtime::RuntimeContext& rt, std::string data) {
    auto buf = std::make_unique<std::string>(std::move(data));
    Runtime::Value ptr_val = rt.make_bytes_ptr(buf->data(), buf->size());
    byte_buffers_.push_back(std::move(buf));
    return ptr_val;
}

uint64_t CapabilityHost::store_ast(Ast::ExprPtr expr) {
    size_t idx = ast_pool_.size();
    ast_pool_.push_back(std::move(expr));
    return encode_handle(HandleTag::Ast, idx);
}

Ast::ExprPtr CapabilityHost::load_ast(uint64_t handle) const {
    return ast_pool_[decode_handle(handle, HandleTag::Ast, ast_pool_.size(), "Ast")];
}

uint64_t CapabilityHost::store_peg(Peg::PegExprPtr expr) {
    size_t idx = peg_pool_.size();
    peg_pool_.push_back(std::move(expr));
    return encode_handle(HandleTag::Peg, idx);
}

Peg::PegExprPtr CapabilityHost::load_peg(uint64_t handle) const {
    return peg_pool_[decode_handle(handle, HandleTag::Peg, peg_pool_.size(), "PegExpr")];
}

uint64_t CapabilityHost::store_frontend(std::unique_ptr<Parser::Frontend> fe) {
    size_t idx = frontend_pool_.size();
    frontend_pool_.push_back(std::move(fe));
    return encode_handle(HandleTag::Frontend, idx);
}

Parser::Frontend* CapabilityHost::load_frontend(uint64_t handle) const {
    return frontend_pool_[decode_handle(handle, HandleTag::Frontend, frontend_pool_.size(), "Frontend")].get();
}

const Peg::Evaluator<Ast::ExprPtr>::Context* CapabilityHost::load_cst(uint64_t handle) const {
    const auto* ctx = cst_pool_[decode_handle(handle, HandleTag::Cst, cst_pool_.size(), "CST")];
    if (!ctx) {
        throw std::runtime_error("capabilities.syntax: CST handle is no longer active");
    }
    return ctx;
}

Runtime::Value CapabilityHost::build_struct(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& fields) {
    Runtime::ConstructingFrame tmp;
    for (const auto& [name, val] : fields) {
        Runtime::FrameSlot* slot = tmp.allocate_val(val, false);
        tmp.bind(rt.symbols.intern(name), slot->pointer());
    }
    return tmp.close_and_flatten(rt.shapes);
}

Runtime::Value CapabilityHost::native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn) {
    uint64_t addr = rt.prototypes.create_native(std::move(name), std::move(fn));
    return rt.make_proto(addr);
}

void CapabilityHost::mount_capabilities(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& modules) {
    Runtime::Value capabilities_obj = build_struct(rt, modules);
    Runtime::ConstructingFrame* root = rt.current_frame();
    Runtime::FrameSlot* slot = root->allocate_val(capabilities_obj, false);
    root->bind(rt.symbols.intern("capabilities"), slot->pointer());
}

void CapabilityHost::install_runtime(Runtime::RuntimeContext& rt) {
    mount_capabilities(rt, {
        {"constants", build_constants_module(rt)},
        {"field_map", build_field_map_module(rt)},
        {"symbols", build_symbols_module(rt)},
        {"intrinsics", build_intrinsics_module(rt)},
    });
}

void CapabilityHost::install_all(Runtime::RuntimeContext& rt) {
    mount_capabilities(rt, {
        {"constants", build_constants_module(rt)},
        {"field_map", build_field_map_module(rt)},
        {"symbols", build_symbols_module(rt)},
        {"intrinsics", build_intrinsics_module(rt)},
        {"ast", build_ast_module(rt)},
        {"syntax", build_syntax_module(rt)},
        {"compiler", build_compiler_module(rt)},
        {"io", build_io_module(rt)},
    });
}

void CapabilityHost::install(Runtime::RuntimeContext& rt) {
    install_all(rt);
}

} // namespace reverSilly::Lang::Eval
