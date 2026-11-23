#include "Capabilities.h"

#include <ffi.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace reverSilly::Lang::Eval::CapabilitiesDetail {

namespace {

struct FfiCallState {
    void* fn_ptr = nullptr;
    ffi_cif cif{};
    ffi_type* ret_type = nullptr;
    uint64_t ret_shape_addr = 0;
    uint64_t ret_size = 0;
    std::vector<Runtime::FieldEntry> param_entries;
    std::vector<ffi_type*> arg_types;
    std::vector<std::unique_ptr<ffi_type>> owned_structs;
    std::vector<std::vector<ffi_type*>> owned_elements;
};

bool has_zero_byte_tag(const Runtime::ShapeLayout* s, Runtime::SymbolId tag_sym) noexcept {
    if (tag_sym == 0) return false;
    const Runtime::FieldEntry* fe = s->fields.find(tag_sym);
    if (!fe) return false;
    const auto* sub = reinterpret_cast<const Runtime::ShapeLayout*>(fe->sub_shape_addr());
    return sub && sub->size == 0;
}

ffi_type* map_shape_to_ffi_type(
    const Runtime::ShapeLayout* s,
    Runtime::SymbolPool& syms,
    bool is_return,
    FfiCallState& state) {
    if (!s) {
        throw std::runtime_error("capabilities.ffi: null ShapeLayout in ABI");
    }

    if (s->size == 0 && s->is_leaf()) {
        if (is_return) {
            return &ffi_type_void;
        }
        throw std::runtime_error("capabilities.ffi: void (0-byte) Shape is not allowed as a C parameter");
    }

    // Float ABI tags (__f32_tag / __f64_tag) disambiguate XMM/VFP vs GPR register classes.
    if (s->size == 4 && has_zero_byte_tag(s, syms.intern("__f32_tag"))) {
        return &ffi_type_float;
    }
    if (s->size == 8 && has_zero_byte_tag(s, syms.intern("__f64_tag"))) {
        return &ffi_type_double;
    }

    // Collect non-zero-sized fields (skipping 0B tag fields like __sym_tag, __bool_tag, __ptr_tag).
    std::vector<const Runtime::FieldEntry*> non_zero;
    if (!s->is_leaf()) {
        const Runtime::FieldEntry* arr = s->fields.entries_begin();
        for (uint32_t i = 0; i < s->fields.count; ++i) {
            const auto* sub = reinterpret_cast<const Runtime::ShapeLayout*>(arr[i].sub_shape_addr());
            if (sub && sub->size > 0) {
                non_zero.push_back(&arr[i]);
            }
        }
    }

    bool is_scalar = s->is_leaf() ||
                     (non_zero.size() == 1 && non_zero[0]->offset() == 0 &&
                      reinterpret_cast<const Runtime::ShapeLayout*>(non_zero[0]->sub_shape_addr())->size == s->size &&
                      reinterpret_cast<const Runtime::ShapeLayout*>(non_zero[0]->sub_shape_addr())->is_leaf());

    if (is_scalar) {
        switch (s->size) {
            case 1: return &ffi_type_uint8;
            case 2: return &ffi_type_uint16;
            case 4: return &ffi_type_uint32;
            case 8: return &ffi_type_uint64;
            default:
                throw std::runtime_error(
                    "capabilities.ffi: unsupported scalar leaf size " + std::to_string(s->size));
        }
    }

    if (non_zero.empty()) {
        throw std::runtime_error("capabilities.ffi: composite struct has no non-zero-sized fields");
    }

    std::vector<ffi_type*> elems;
    elems.reserve(non_zero.size() + 1);
    for (const Runtime::FieldEntry* fe : non_zero) {
        const auto* sub = reinterpret_cast<const Runtime::ShapeLayout*>(fe->sub_shape_addr());
        elems.push_back(map_shape_to_ffi_type(sub, syms, /*is_return=*/false, state));
    }
    elems.push_back(nullptr);

    auto st = std::make_unique<ffi_type>();
    st->size = 0;
    st->alignment = 0;
    st->type = FFI_TYPE_STRUCT;
    state.owned_elements.push_back(std::move(elems));
    st->elements = state.owned_elements.back().data();

    // Verify exact C ABI struct size and per-field byte offsets against Lang's ShapeLayout.
    std::vector<size_t> offsets(non_zero.size(), 0);
    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, st.get(), offsets.data()) != FFI_OK) {
        throw std::runtime_error("capabilities.ffi: ffi_get_struct_offsets failed");
    }
    if (st->size != s->size) {
        throw std::runtime_error(
            "capabilities.ffi: struct size mismatch (Lang=" + std::to_string(s->size) +
            ", C ABI=" + std::to_string(st->size) + "); pass by pointer instead");
    }
    for (size_t i = 0; i < non_zero.size(); ++i) {
        if (offsets[i] != non_zero[i]->offset()) {
            throw std::runtime_error(
                "capabilities.ffi: struct field '" + std::string(syms.name(non_zero[i]->sym)) +
                "' offset mismatch (Lang=" + std::to_string(non_zero[i]->offset()) +
                ", C ABI=" + std::to_string(offsets[i]) + "); pass by pointer instead");
        }
    }

    ffi_type* raw = st.get();
    state.owned_structs.push_back(std::move(st));
    return raw;
}

Runtime::Value read_abi_field(
    Runtime::RuntimeContext& rt,
    const Runtime::PointerLayout& abi_ptr,
    std::string_view name,
    uint64_t expected_shape) {
    Runtime::PointerLayout fp = rt.field(abi_ptr, rt.symbols.intern(name));
    if (fp.is_null()) {
        throw std::runtime_error("capabilities.ffi.load_symbol: abi missing field '" + std::string(name) + "'");
    }
    if (fp.shape_ptr != expected_shape) {
        throw std::runtime_error("capabilities.ffi.load_symbol: abi field '" + std::string(name) + "' has wrong Shape");
    }
    return rt.deref(fp);
}

} // namespace

Runtime::Value build_ffi_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"open_lib", native_proto(rt, "capabilities.ffi.open_lib", [](Evaluator& ev) {
            auto path_span = read_bytes_ptr(ev, "path");
            std::string path(reinterpret_cast<const char*>(path_span.data()), path_span.size());

            void* handle = nullptr;
#ifdef _WIN32
            if (path.empty()) {
                handle = static_cast<void*>(GetModuleHandleA(nullptr));
            } else {
                handle = static_cast<void*>(LoadLibraryA(path.c_str()));
            }
            if (!handle) {
                throw std::runtime_error("capabilities.ffi.open_lib: failed to load '" + path + "'");
            }
#else
            handle = dlopen(path.empty() ? nullptr : path.c_str(), RTLD_LAZY);
            if (!handle) {
                const char* err = dlerror();
                throw std::runtime_error(
                    "capabilities.ffi.open_lib: failed to load '" + path + "': " + (err ? err : "unknown error"));
            }
#endif
            return ev.rt.make_handle(reinterpret_cast<uint64_t>(handle));
        })},

        {"close_lib", native_proto(rt, "capabilities.ffi.close_lib", [](Evaluator& ev) {
            uint64_t raw = read_field(ev, "lib").as_pod<uint64_t>();
            if (raw == 0) {
                return ev.rt.make_bool(false);
            }
            void* handle = reinterpret_cast<void*>(raw);
#ifdef _WIN32
            return ev.rt.make_bool(FreeLibrary(static_cast<HMODULE>(handle)) != 0);
#else
            return ev.rt.make_bool(dlclose(handle) == 0);
#endif
        })},

        {"load_symbol", native_proto(rt, "capabilities.ffi.load_symbol", [](Evaluator& ev) {
            uint64_t raw_lib = read_field(ev, "lib").as_pod<uint64_t>();
            if (raw_lib == 0) {
                throw std::runtime_error("capabilities.ffi.load_symbol: null library handle");
            }
            void* lib_handle = reinterpret_cast<void*>(raw_lib);

            auto name_span = read_bytes_ptr(ev, "name");
            std::string sym_name(reinterpret_cast<const char*>(name_span.data()), name_span.size());
            if (sym_name.empty()) {
                throw std::runtime_error("capabilities.ffi.load_symbol: symbol name cannot be empty");
            }

            Runtime::Value abi_val = read_field(ev, "abi");
            if (abi_val.shape_addr == 0 || abi_val.bytes.empty()) {
                throw std::runtime_error("capabilities.ffi.load_symbol: invalid abi struct");
            }
            Runtime::PointerLayout abi_ptr{
                reinterpret_cast<uint64_t>(abi_val.bytes.data()), abi_val.shape_addr, 0};

            auto args_sl = read_abi_field(ev.rt, abi_ptr, "args", ev.rt.shapes.shape_meta).as_pod<Runtime::ShapeLayout>();
            auto ret_sl = read_abi_field(ev.rt, abi_ptr, "ret", ev.rt.shapes.shape_meta).as_pod<Runtime::ShapeLayout>();
            auto cc_sym = read_abi_field(ev.rt, abi_ptr, "cc", ev.rt.shapes.symbol_shape).as_pod<Runtime::SymbolId>();

            if (ev.rt.symbols.name(cc_sym) != "c") {
                throw std::runtime_error(
                    "capabilities.ffi.load_symbol: unsupported calling convention '" +
                    std::string(ev.rt.symbols.name(cc_sym)) + "' (only 'c' is supported)");
            }

            uint64_t args_addr = ev.canonical_shape_addr(args_sl);
            uint64_t ret_addr = ev.canonical_shape_addr(ret_sl);
            const auto* args_shape = reinterpret_cast<const Runtime::ShapeLayout*>(args_addr);
            const auto* ret_shape = reinterpret_cast<const Runtime::ShapeLayout*>(ret_addr);

            void* fn_ptr = nullptr;
#ifdef _WIN32
            fn_ptr = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_handle), sym_name.c_str()));
            if (!fn_ptr) {
                throw std::runtime_error("capabilities.ffi.load_symbol: symbol '" + sym_name + "' not found");
            }
#else
            (void)dlerror();
            fn_ptr = dlsym(lib_handle, sym_name.c_str());
            const char* dl_err = dlerror();
            if (dl_err != nullptr || fn_ptr == nullptr) {
                throw std::runtime_error(
                    "capabilities.ffi.load_symbol: symbol '" + sym_name + "' not found: " +
                    (dl_err ? dl_err : "null address"));
            }
#endif

            auto state = std::make_shared<FfiCallState>();
            state->fn_ptr = fn_ptr;
            state->ret_shape_addr = ret_addr;
            state->ret_size = ret_shape->size;
            state->ret_type = map_shape_to_ffi_type(ret_shape, ev.rt.symbols, /*is_return=*/true, *state);

            const Runtime::FieldEntry* entries = args_shape->fields.entries_begin();
            state->param_entries.reserve(args_shape->fields.count);
            state->arg_types.reserve(args_shape->fields.count);
            for (uint32_t i = 0; i < args_shape->fields.count; ++i) {
                const auto* param_shape = reinterpret_cast<const Runtime::ShapeLayout*>(entries[i].sub_shape_addr());
                if (param_shape && param_shape->size == 0) {
                    continue; // skip 0B tag fields if present on args struct
                }
                state->param_entries.push_back(entries[i]);
                state->arg_types.push_back(
                    map_shape_to_ffi_type(param_shape, ev.rt.symbols, /*is_return=*/false, *state));
            }

            ffi_status st = ffi_prep_cif(
                &state->cif,
                FFI_DEFAULT_ABI,
                static_cast<unsigned int>(state->arg_types.size()),
                state->ret_type,
                state->arg_types.empty() ? nullptr : state->arg_types.data());
            if (st != FFI_OK) {
                throw std::runtime_error(
                    "capabilities.ffi.load_symbol: ffi_prep_cif failed for '" + sym_name + "'");
            }

            Runtime::FfiBinding binding;
            binding.symbol_name = sym_name;
            binding.args_shape_addr = args_addr;
            binding.ret_shape_addr = ret_addr;
            binding.cc = cc_sym;

            uint64_t proto_addr = ev.rt.prototypes.create_ffi(
                std::move(binding),
                [state](Evaluator& call_ev) -> Runtime::Value {
                    Runtime::ConstructingFrame* f = call_ev.rt.current_frame();
                    if (!f) {
                        throw std::runtime_error("capabilities.ffi: no active frame");
                    }

                    std::vector<void*> arg_ptrs(state->param_entries.size(), nullptr);
                    for (size_t i = 0; i < state->param_entries.size(); ++i) {
                        const Runtime::FieldEntry& fe = state->param_entries[i];
                        Runtime::PointerLayout p = f->field(fe.sym);
                        if (p.is_null()) {
                            throw std::runtime_error(
                                "capabilities.ffi: missing required argument '" +
                                std::string(call_ev.rt.symbols.name(fe.sym)) + "' in #this");
                        }
                        if (p.shape_ptr != fe.sub_shape_addr()) {
                            throw std::runtime_error(
                                "capabilities.ffi: Shape mismatch for argument '" +
                                std::string(call_ev.rt.symbols.name(fe.sym)) + "'");
                        }
                        if (Runtime::FrameSlot* slot = f->find_direct_slot(p.addr)) {
                            if (slot->state != Runtime::SlotState::Live) {
                                throw std::runtime_error(
                                    "capabilities.ffi: argument '" +
                                    std::string(call_ev.rt.symbols.name(fe.sym)) + "' is not Live");
                            }
                        }
                        arg_ptrs[i] = reinterpret_cast<void*>(p.addr);
                    }

                    if (state->ret_size == 0) {
                        ffi_call(&state->cif, FFI_FN(state->fn_ptr), nullptr,
                                 arg_ptrs.empty() ? nullptr : arg_ptrs.data());
                        return call_ev.rt.make_unit();
                    }

                    Runtime::Value out;
                    out.shape_addr = state->ret_shape_addr;
                    out.bytes.resize(state->ret_size, 0);

                    if (state->ret_type == &ffi_type_uint8 ||
                        state->ret_type == &ffi_type_uint16 ||
                        state->ret_type == &ffi_type_uint32) {
                        ffi_arg rval = 0;
                        ffi_call(&state->cif, FFI_FN(state->fn_ptr), &rval,
                                 arg_ptrs.empty() ? nullptr : arg_ptrs.data());
                        if (state->ret_type == &ffi_type_uint8) {
                            auto v = static_cast<uint8_t>(rval);
                            std::memcpy(out.bytes.data(), &v, sizeof(v));
                        } else if (state->ret_type == &ffi_type_uint16) {
                            auto v = static_cast<uint16_t>(rval);
                            std::memcpy(out.bytes.data(), &v, sizeof(v));
                        } else {
                            auto v = static_cast<uint32_t>(rval);
                            std::memcpy(out.bytes.data(), &v, sizeof(v));
                        }
                        return out;
                    }

                    // Ensure return scratch buffer is at least sizeof(ffi_arg) bytes for libffi safety.
                    std::vector<uint8_t> scratch(std::max<size_t>(state->ret_size, sizeof(ffi_arg)), 0);
                    ffi_call(&state->cif, FFI_FN(state->fn_ptr), scratch.data(),
                             arg_ptrs.empty() ? nullptr : arg_ptrs.data());
                    std::memcpy(out.bytes.data(), scratch.data(), state->ret_size);
                    return out;
                });

            return ev.rt.make_proto(proto_addr);
        })},
    });
}

} // namespace reverSilly::Lang::Eval::CapabilitiesDetail
