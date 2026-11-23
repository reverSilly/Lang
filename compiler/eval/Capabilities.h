#pragma once

#include "Evaluator.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reverSilly::Lang::Eval {

// Stateless capability installers (zero hidden host state).
//   - install_runtime_capabilities: Tier 1 only (constants, field_map, symbols, intrinsics)
//   - install_runtime_with_ffi:     Tier 1 + Tier 2 (adds ffi)
//   - install_all_capabilities:     Tier 1 + Tier 2 + Tier 3 (adds ast, syntax, compiler, temporary io)
void install_runtime_capabilities(Runtime::RuntimeContext& rt);
void install_runtime_with_ffi(Runtime::RuntimeContext& rt);
void install_all_capabilities(Runtime::RuntimeContext& rt);

namespace CapabilitiesDetail {

Runtime::Value read_field(Evaluator& ev, std::string_view name);
std::span<const uint8_t> read_bytes_ptr(Evaluator& ev, std::string_view name);
std::vector<Runtime::Value> read_positional(Evaluator& ev);
Runtime::Value freeze_string_bytes(Runtime::RuntimeContext& rt, std::string_view data);
Runtime::Value build_struct(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& fields);
Runtime::Value native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn);

// Tier 1: Runtime Core (RuntimeCaps.cpp)
Runtime::Value build_constants_module(Runtime::RuntimeContext& rt);
Runtime::Value build_field_map_module(Runtime::RuntimeContext& rt);
Runtime::Value build_symbols_module(Runtime::RuntimeContext& rt);
Runtime::Value build_intrinsics_module(Runtime::RuntimeContext& rt);

// Tier 2: Host FFI (FfiCaps.cpp)
Runtime::Value build_ffi_module(Runtime::RuntimeContext& rt);

// Tier 3: Compiler Frontend (CompilerCaps.cpp)
Runtime::Value build_ast_module(Runtime::RuntimeContext& rt);
Runtime::Value build_syntax_module(Runtime::RuntimeContext& rt);
Runtime::Value build_compiler_module(Runtime::RuntimeContext& rt);
Runtime::Value build_io_module(Runtime::RuntimeContext& rt);

} // namespace CapabilitiesDetail

} // namespace reverSilly::Lang::Eval
