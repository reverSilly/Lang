#include "Capabilities.h"

#include <stdexcept>
#include <string>

namespace reverSilly::Lang::Eval::CapabilitiesDetail {

namespace {

struct ParsedAbi {
    uint64_t args_addr = 0;
    uint64_t ret_addr = 0;
    Runtime::SymbolId cc = 0;
};

Runtime::Value read_abi_field(
    Runtime::RuntimeContext& rt,
    const Runtime::PointerLayout& abi_ptr,
    std::string_view name,
    uint64_t expected_shape,
    std::string_view ctx_name) {
    Runtime::PointerLayout fp = rt.field(abi_ptr, rt.symbols.intern(name));
    if (fp.is_null()) {
        throw std::runtime_error(std::string(ctx_name) + ": abi missing field '" + std::string(name) + "'");
    }
    if (fp.shape_ptr != expected_shape) {
        throw std::runtime_error(std::string(ctx_name) + ": abi field '" + std::string(name) + "' has wrong Shape");
    }
    return rt.deref(fp);
}

ParsedAbi read_and_validate_abi(Evaluator& ev, std::string_view ctx_name) {
    Runtime::Value abi_val = read_field(ev, "abi");
    if (abi_val.shape_addr == 0 || abi_val.bytes.empty()) {
        throw std::runtime_error(std::string(ctx_name) + ": invalid abi struct");
    }
    Runtime::PointerLayout abi_ptr{
        reinterpret_cast<uint64_t>(abi_val.bytes.data()), abi_val.shape_addr, 0};

    auto read_shape_ptr = [&](std::string_view field_name) -> uint64_t {
        auto sp = read_abi_field(ev.rt, abi_ptr, field_name, ev.rt.shapes.pointer_shape, ctx_name)
                      .as_pod<Runtime::PointerLayout>();
        if (sp.shape_ptr != ev.rt.shapes.shape_meta || sp.addr == 0 || !ev.rt.shapes.owns(sp.addr)) {
            throw std::runtime_error(
                std::string(ctx_name) + ": abi field '" + std::string(field_name) +
                "' must be a valid Pointer(Shape) in ShapeArena");
        }
        return sp.addr;
    };
    uint64_t args_addr = read_shape_ptr("args");
    uint64_t ret_addr = read_shape_ptr("ret");
    auto cc_sym = read_abi_field(ev.rt, abi_ptr, "cc", ev.rt.shapes.symbol_shape, ctx_name)
                      .as_pod<Runtime::SymbolId>();

    ev.rt.ffi_cache.validate_abi(ev.rt, args_addr, ret_addr, cc_sym);
    return ParsedAbi{args_addr, ret_addr, cc_sym};
}

} // namespace

Runtime::Value build_ffi_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        // Pure-data declarative external C symbol factory (stageable into Standalone Ast).
        {"extern_symbol", native_proto(rt, "capabilities.ffi.extern_symbol", [](Evaluator& ev) {
            auto lib_bytes = read_raw_bytes(ev, "lib");
            auto name_bytes = read_raw_bytes(ev, "name");
            std::string lib_name(reinterpret_cast<const char*>(lib_bytes.data()), lib_bytes.size());
            std::string sym_name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
            if (sym_name.empty()) {
                throw std::runtime_error("capabilities.ffi.extern_symbol: symbol name cannot be empty");
            }
            ParsedAbi abi = read_and_validate_abi(ev, "capabilities.ffi.extern_symbol");
            Runtime::FfiBinding binding{
                std::move(lib_name), std::move(sym_name), abi.args_addr, abi.ret_addr, abi.cc};
            return ev.rt.make_proto(ev.rt.prototypes.create_ffi_extern(std::move(binding)));
        })},

        // Two-stage indirect C function pointer binder factory (stageable into Standalone Ast).
        // Stage 1: bind_ptr({ fn: Symbol, abi }) -> FfiBindPtrProto
        // Stage 2: #evaluate(binder) reads #this.<fn> (8B c_ptr) -> BoundFfiProto
        // Stage 3: #evaluate(bound) reads abi.args from #this and invokes fn_ptr
        {"bind_ptr", native_proto(rt, "capabilities.ffi.bind_ptr", [](Evaluator& ev) {
            Runtime::Value fn_val = read_field(ev, "fn");
            if (fn_val.shape_addr != ev.rt.shapes.symbol_shape) {
                throw std::runtime_error("capabilities.ffi.bind_ptr: 'fn' must be a Symbol");
            }
            auto fn_slot = fn_val.as_pod<Runtime::SymbolId>();
            ParsedAbi abi = read_and_validate_abi(ev, "capabilities.ffi.bind_ptr");
            Runtime::FfiBindPtrSpec spec{fn_slot, abi.args_addr, abi.ret_addr, abi.cc};
            return ev.rt.make_proto(ev.rt.prototypes.create_ffi_bind_ptr(spec));
        })},
    });
}

} // namespace reverSilly::Lang::Eval::CapabilitiesDetail
