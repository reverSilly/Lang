#pragma once

#include "CliArgs.h"
#include "Evaluator.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reverSilly::Lang::Eval {

// Stateless capability installers (zero hidden host state).
//   - install_runtime_capabilities: Tier 1 only (constants, field_map, shapes, symbols, intrinsics)
//   - install_runtime_with_ffi:     Tier 1 + Tier 2 (adds ffi)
//   - install_all_capabilities:     Tier 1 + Tier 2 + Tier 3 (adds ast, syntax, backends, temporary io)
void install_runtime_capabilities(Runtime::RuntimeContext& rt);
void install_runtime_with_ffi(Runtime::RuntimeContext& rt);
void install_all_capabilities(Runtime::RuntimeContext& rt);

namespace CapabilitiesDetail {

Runtime::Value read_field(Evaluator& ev, std::string_view name);
std::vector<uint8_t> read_raw_bytes(Evaluator& ev, std::string_view name);
std::vector<Runtime::Value> read_positional(Evaluator& ev);
Runtime::Value build_struct(
    Runtime::RuntimeContext& rt,
    const std::vector<std::pair<std::string_view, Runtime::Value>>& fields);
Runtime::Value native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn);

// Tier 1: Core Factories & Reflection (RuntimeCaps.cpp)
Runtime::Value build_constants_module(Runtime::RuntimeContext& rt);
Runtime::Value build_field_map_module(Runtime::RuntimeContext& rt);
Runtime::Value build_shapes_module(Runtime::RuntimeContext& rt);
Runtime::Value build_symbols_module(Runtime::RuntimeContext& rt);
Runtime::Value build_intrinsics_module(Runtime::RuntimeContext& rt);

// Tier 2: Unified Declarative FFI (FfiCaps.cpp)
Runtime::Value build_ffi_module(Runtime::RuntimeContext& rt);

// Tier 3: Compiler Frontend & Backends (CompilerCaps.cpp)
Runtime::Value build_ast_module(Runtime::RuntimeContext& rt);
Runtime::Value build_syntax_module(Runtime::RuntimeContext& rt);
Runtime::Value build_backends_module(Runtime::RuntimeContext& rt);
Runtime::Value build_io_module(Runtime::RuntimeContext& rt);

} // namespace CapabilitiesDetail

} // namespace reverSilly::Lang::Eval
