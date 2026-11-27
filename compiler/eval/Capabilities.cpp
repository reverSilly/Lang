#include "Capabilities.h"

#include <stdexcept>

namespace reverSilly::Lang::Eval {

namespace CapabilitiesDetail {

Runtime::Value read_field(Evaluator& ev, std::string_view name) {
    Runtime::ConstructingFrame* f = ev.rt.current_frame();
    Runtime::SymbolId sym = ev.rt.symbols.intern(name);
    Runtime::PointerLayout p = f->field(sym);
    if (p.is_null()) {
        throw std::runtime_error("capability missing required field '" + std::string(name) + "' in #this");
    }
    return ev.rt.deref(p);
}

std::vector<uint8_t> read_raw_bytes(Evaluator& ev, std::string_view name) {
    Runtime::Value v = read_field(ev, name);
    const auto* sl = v.shape();
    if (!sl || !sl->is_leaf() || sl->align != 1) {
        throw std::runtime_error(
            "capabilities: field '" + std::string(name) + "' must be a RawBytes leaf value (align = 1)");
    }
    return std::move(v.bytes);
}

std::vector<Runtime::Value> read_positional(Evaluator& ev) {
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

Runtime::Value build_struct(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& fields) {
    Runtime::ConstructingFrame tmp;
    for (const auto& [name, val] : fields) {
        Runtime::FrameSlot* slot = tmp.allocate_val(val, false);
        tmp.bind(rt.symbols.intern(name), slot->pointer());
    }
    return tmp.close_and_flatten(rt.shapes);
}

Runtime::Value native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn) {
    uint64_t addr = rt.prototypes.create_native(std::move(name), std::move(fn));
    return rt.make_proto(addr);
}

} // namespace CapabilitiesDetail

namespace {

void mount_capabilities(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& modules) {
    Runtime::Value capabilities_obj = CapabilitiesDetail::build_struct(rt, modules);
    Runtime::ConstructingFrame* root = rt.current_frame();
    Runtime::FrameSlot* slot = root->allocate_val(capabilities_obj, false);
    root->bind(rt.symbols.intern("capabilities"), slot->pointer());
}

} // namespace

void install_runtime_capabilities(Runtime::RuntimeContext& rt) {
    using namespace CapabilitiesDetail;
    mount_capabilities(rt, {
        {"constants", build_constants_module(rt)},
        {"field_map", build_field_map_module(rt)},
        {"shapes", build_shapes_module(rt)},
        {"symbols", build_symbols_module(rt)},
        {"intrinsics", build_intrinsics_module(rt)},
    });
}

void install_runtime_with_ffi(Runtime::RuntimeContext& rt) {
    using namespace CapabilitiesDetail;
    mount_capabilities(rt, {
        {"constants", build_constants_module(rt)},
        {"field_map", build_field_map_module(rt)},
        {"shapes", build_shapes_module(rt)},
        {"symbols", build_symbols_module(rt)},
        {"intrinsics", build_intrinsics_module(rt)},
        {"ffi", build_ffi_module(rt)},
    });
}

void install_all_capabilities(Runtime::RuntimeContext& rt) {
    using namespace CapabilitiesDetail;
    mount_capabilities(rt, {
        {"constants", build_constants_module(rt)},
        {"field_map", build_field_map_module(rt)},
        {"shapes", build_shapes_module(rt)},
        {"symbols", build_symbols_module(rt)},
        {"intrinsics", build_intrinsics_module(rt)},
        {"ffi", build_ffi_module(rt)},
        {"ast", build_ast_module(rt)},
        {"syntax", build_syntax_module(rt)},
        {"backends", build_backends_module(rt)},
        {"io", build_io_module(rt)},
    });
}

} // namespace reverSilly::Lang::Eval
