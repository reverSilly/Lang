#pragma once

#include "Prototype.h"

#include <cstdint>
#include <memory>

namespace reverSilly::Lang::Runtime {

class RuntimeContext;

// Lazy per-RuntimeContext FFI dispatch cache (Phase 1 interpreter).
// Owns lazily opened OS dynamic libraries (automatically closed in destructor)
// and prepared libffi CIFs keyed by canonical (args_shape_addr, ret_shape_addr, cc).
class FfiDispatchCache {
public:
    FfiDispatchCache();
    ~FfiDispatchCache();

    FfiDispatchCache(const FfiDispatchCache&) = delete;
    FfiDispatchCache& operator=(const FfiDispatchCache&) = delete;

    // Validate ABI shapes & struct layouts eagerly (called by ffi.extern_symbol / ffi.bind_ptr).
    void validate_abi(RuntimeContext& rt, uint64_t args_addr, uint64_t ret_addr, SymbolId cc);

    // Invoke a declarative external symbol (lazily opens lib & resolves symbol in this RuntimeContext).
    Value call_extern(RuntimeContext& rt, const FfiBinding& binding);

    // Invoke a runtime-bound C function pointer (Stage 3 of ffi.bind_ptr).
    Value call_bound(RuntimeContext& rt, const BoundFfiSpec& spec);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reverSilly::Lang::Runtime
