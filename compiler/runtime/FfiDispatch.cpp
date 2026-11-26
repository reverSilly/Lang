#include "FfiDispatch.h"
#include "Runtime.h"

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
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Runtime {

namespace {

struct PreparedCif {
    ffi_cif cif{};
    ffi_type* ret_type = nullptr;
    uint64_t ret_shape_addr = 0;
    uint64_t ret_size = 0;
    std::vector<FieldEntry> param_entries;
    std::vector<ffi_type*> arg_types;
    std::vector<std::unique_ptr<ffi_type>> owned_structs;
    std::vector<std::vector<ffi_type*>> owned_elements;
};

struct CifKey {
    uint64_t args_addr;
    uint64_t ret_addr;
    SymbolId cc;
    bool operator==(const CifKey&) const noexcept = default;
};

struct CifKeyHash {
    size_t operator()(const CifKey& k) const noexcept {
        size_t h = std::hash<uint64_t>{}(k.args_addr);
        h ^= std::hash<uint64_t>{}(k.ret_addr) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.cc) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct SymKey {
    std::string lib_name;
    std::string sym_name;
    bool operator==(const SymKey&) const noexcept = default;
};

struct SymKeyHash {
    size_t operator()(const SymKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.lib_name);
        h ^= std::hash<std::string>{}(k.sym_name) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

bool has_zero_byte_tag(const ShapeLayout* s, SymbolId tag_sym) noexcept {
    if (tag_sym == 0) return false;
    const FieldEntry* fe = s->fields.find(tag_sym);
    if (!fe) return false;
    const auto* sub = reinterpret_cast<const ShapeLayout*>(fe->sub_shape_addr());
    return sub && sub->size == 0;
}

ffi_type* map_shape_to_ffi_type(
    const ShapeLayout* s,
    SymbolPool& syms,
    bool is_return,
    PreparedCif& state) {
    if (!s) {
        throw std::runtime_error("ffi: null ShapeLayout in ABI");
    }

    if (s->size == 0 && s->is_leaf()) {
        if (is_return) {
            return &ffi_type_void;
        }
        throw std::runtime_error("ffi: void (0-byte) Shape is not allowed as a C parameter");
    }

    // Float ABI tags (__f32_tag / __f64_tag) disambiguate XMM/VFP vs GPR register classes.
    if (s->size == 4 && has_zero_byte_tag(s, syms.intern("__f32_tag"))) {
        return &ffi_type_float;
    }
    if (s->size == 8 && has_zero_byte_tag(s, syms.intern("__f64_tag"))) {
        return &ffi_type_double;
    }

    if (s->is_leaf()) {
        switch (s->size) {
            case 1: return &ffi_type_uint8;
            case 2: return &ffi_type_uint16;
            case 4: return &ffi_type_uint32;
            case 8: return &ffi_type_uint64;
            default:
                throw std::runtime_error(
                    "ffi: unsupported scalar leaf size " + std::to_string(s->size));
        }
    }

    std::vector<const FieldEntry*> non_zero;
    const FieldEntry* arr = s->fields.entries_begin();
    for (uint32_t i = 0; i < s->fields.count; ++i) {
        const auto* sub = reinterpret_cast<const ShapeLayout*>(arr[i].sub_shape_addr());
        if (sub && sub->size > 0) {
            non_zero.push_back(&arr[i]);
        }
    }
    if (non_zero.empty()) {
        throw std::runtime_error("ffi: composite struct has no non-zero-sized fields");
    }

    std::vector<ffi_type*> elems;
    elems.reserve(non_zero.size() + 1);
    for (const FieldEntry* fe : non_zero) {
        const auto* sub = reinterpret_cast<const ShapeLayout*>(fe->sub_shape_addr());
        elems.push_back(map_shape_to_ffi_type(sub, syms, /*is_return=*/false, state));
    }
    elems.push_back(nullptr);

    auto st = std::make_unique<ffi_type>();
    st->size = 0;
    st->alignment = 0;
    st->type = FFI_TYPE_STRUCT;
    state.owned_elements.push_back(std::move(elems));
    st->elements = state.owned_elements.back().data();

    std::vector<size_t> offsets(non_zero.size(), 0);
    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, st.get(), offsets.data()) != FFI_OK) {
        throw std::runtime_error("ffi: ffi_get_struct_offsets failed");
    }
    if (st->size != s->size) {
        throw std::runtime_error(
            "ffi: struct size mismatch (Lang=" + std::to_string(s->size) +
            ", C ABI=" + std::to_string(st->size) + "); pass by pointer instead");
    }
    for (size_t i = 0; i < non_zero.size(); ++i) {
        if (offsets[i] != non_zero[i]->offset()) {
            throw std::runtime_error(
                "ffi: struct field '" + std::string(syms.name(non_zero[i]->sym)) +
                "' offset mismatch (Lang=" + std::to_string(non_zero[i]->offset()) +
                ", C ABI=" + std::to_string(offsets[i]) + "); pass by pointer instead");
        }
    }

    ffi_type* raw = st.get();
    state.owned_structs.push_back(std::move(st));
    return raw;
}

Value invoke_prepared(RuntimeContext& rt, const PreparedCif& state, void* fn_ptr) {
    if (!fn_ptr) {
        throw std::runtime_error("ffi: null C function pointer");
    }
    ConstructingFrame* f = rt.current_frame();
    if (!f) {
        throw std::runtime_error("ffi: no active frame");
    }

    const size_t nargs = state.param_entries.size();
    std::vector<void*> arg_ptrs(nargs, nullptr);

    for (size_t i = 0; i < nargs; ++i) {
        const FieldEntry& expected = state.param_entries[i];
        PointerLayout slot_ptr = f->field(expected.sym);
        if (slot_ptr.is_null()) {
            throw std::runtime_error(
                "ffi: missing required argument slot '" +
                std::string(rt.symbols.name(expected.sym)) + "' in #this");
        }
        if (slot_ptr.shape_ptr != expected.sub_shape_addr()) {
            throw std::runtime_error(
                "ffi: argument slot '" +
                std::string(rt.symbols.name(expected.sym)) + "' Shape mismatch");
        }
        arg_ptrs[i] = reinterpret_cast<void*>(slot_ptr.addr);
    }

    if (state.ret_size == 0) {
        ffi_call(const_cast<ffi_cif*>(&state.cif), FFI_FN(fn_ptr), nullptr,
                 nargs > 0 ? arg_ptrs.data() : nullptr);
        return rt.make_unit();
    }

    const size_t rbuf_size = std::max<size_t>(state.ret_size, sizeof(ffi_arg));
    alignas(16) std::vector<uint8_t> rbuf(rbuf_size, 0);
    ffi_call(const_cast<ffi_cif*>(&state.cif), FFI_FN(fn_ptr), rbuf.data(),
             nargs > 0 ? arg_ptrs.data() : nullptr);

    Value out;
    out.shape_addr = state.ret_shape_addr;
    out.bytes.assign(rbuf.begin(), rbuf.begin() + static_cast<std::ptrdiff_t>(state.ret_size));
    return out;
}

} // namespace

struct FfiDispatchCache::Impl {
    std::unordered_map<std::string, void*> libs;
    std::unordered_map<SymKey, void*, SymKeyHash> syms;
    std::unordered_map<CifKey, std::shared_ptr<PreparedCif>, CifKeyHash> cifs;

    ~Impl() {
        for (auto& [name, handle] : libs) {
            if (!handle) continue;
#ifdef _WIN32
            if (!name.empty()) {
                FreeLibrary(static_cast<HMODULE>(handle));
            }
#else
            dlclose(handle);
#endif
        }
    }

    const PreparedCif& get_or_prepare_cif(
        RuntimeContext& rt, uint64_t args_addr, uint64_t ret_addr, SymbolId cc) {
        CifKey key{args_addr, ret_addr, cc};
        if (auto it = cifs.find(key); it != cifs.end()) {
            return *it->second;
        }
        if (rt.symbols.name(cc) != "c") {
            throw std::runtime_error(
                "ffi: unsupported calling convention '" +
                std::string(rt.symbols.name(cc)) + "' (only 'c' is supported)");
        }
        const auto* args_shape = reinterpret_cast<const ShapeLayout*>(args_addr);
        const auto* ret_shape = reinterpret_cast<const ShapeLayout*>(ret_addr);

        auto state = std::make_shared<PreparedCif>();
        state->ret_shape_addr = ret_addr;
        state->ret_size = ret_shape ? ret_shape->size : 0;
        state->ret_type = map_shape_to_ffi_type(ret_shape, rt.symbols, /*is_return=*/true, *state);

        if (args_shape && !args_shape->is_leaf()) {
            const FieldEntry* arr = args_shape->fields.entries_begin();
            for (uint32_t i = 0; i < args_shape->fields.count; ++i) {
                const auto* sub = reinterpret_cast<const ShapeLayout*>(arr[i].sub_shape_addr());
                if (!sub || sub->size == 0) continue;
                state->param_entries.push_back(arr[i]);
                state->arg_types.push_back(map_shape_to_ffi_type(sub, rt.symbols, /*is_return=*/false, *state));
            }
        }

        if (ffi_prep_cif(
                &state->cif,
                FFI_DEFAULT_ABI,
                static_cast<unsigned int>(state->arg_types.size()),
                state->ret_type,
                state->arg_types.empty() ? nullptr : state->arg_types.data()) != FFI_OK) {
            throw std::runtime_error("ffi: ffi_prep_cif failed");
        }

        const PreparedCif& ref = *state;
        cifs.emplace(key, std::move(state));
        return ref;
    }

    void* get_or_open_lib(const std::string& lib_name) {
        if (auto it = libs.find(lib_name); it != libs.end()) {
            return it->second;
        }
        void* handle = nullptr;
#ifdef _WIN32
        if (lib_name.empty()) {
            handle = static_cast<void*>(GetModuleHandleA(nullptr));
        } else {
            handle = static_cast<void*>(LoadLibraryA(lib_name.c_str()));
        }
        if (!handle) {
            throw std::runtime_error("ffi: failed to open library '" + lib_name + "'");
        }
#else
        handle = dlopen(lib_name.empty() ? nullptr : lib_name.c_str(), RTLD_LAZY);
        if (!handle) {
            const char* err = dlerror();
            throw std::runtime_error(
                "ffi: failed to open library '" + lib_name + "': " + (err ? err : "unknown error"));
        }
#endif
        libs.emplace(lib_name, handle);
        return handle;
    }

    void* get_or_resolve_sym(const std::string& lib_name, const std::string& sym_name) {
        SymKey key{lib_name, sym_name};
        if (auto it = syms.find(key); it != syms.end()) {
            return it->second;
        }
        void* lib_handle = get_or_open_lib(lib_name);
        void* fn_ptr = nullptr;
#ifdef _WIN32
        fn_ptr = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_handle), sym_name.c_str()));
        if (!fn_ptr) {
            throw std::runtime_error("ffi: symbol '" + sym_name + "' not found in '" + lib_name + "'");
        }
#else
        (void)dlerror();
        fn_ptr = dlsym(lib_handle, sym_name.c_str());
        const char* dl_err = dlerror();
        if (dl_err != nullptr || fn_ptr == nullptr) {
            throw std::runtime_error(
                "ffi: symbol '" + sym_name + "' not found in '" + lib_name + "': " +
                (dl_err ? dl_err : "null address"));
        }
#endif
        syms.emplace(std::move(key), fn_ptr);
        return fn_ptr;
    }
};

FfiDispatchCache::FfiDispatchCache() : impl_(std::make_unique<Impl>()) {}
FfiDispatchCache::~FfiDispatchCache() = default;

void FfiDispatchCache::validate_abi(
    RuntimeContext& rt, uint64_t args_addr, uint64_t ret_addr, SymbolId cc) {
    (void)impl_->get_or_prepare_cif(rt, args_addr, ret_addr, cc);
}

Value FfiDispatchCache::call_extern(RuntimeContext& rt, const FfiBinding& binding) {
    const PreparedCif& cif = impl_->get_or_prepare_cif(
        rt, binding.args_shape_addr, binding.ret_shape_addr, binding.cc);
    void* fn_ptr = impl_->get_or_resolve_sym(binding.lib_name, binding.symbol_name);
    return invoke_prepared(rt, cif, fn_ptr);
}

Value FfiDispatchCache::call_bound(RuntimeContext& rt, const BoundFfiSpec& spec) {
    const PreparedCif& cif = impl_->get_or_prepare_cif(
        rt, spec.args_shape_addr, spec.ret_shape_addr, spec.cc);
    return invoke_prepared(rt, cif, spec.fn_ptr);
}

} // namespace reverSilly::Lang::Runtime
