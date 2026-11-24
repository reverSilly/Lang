#pragma once

#include "../runtime/Runtime.h"

#include <string_view>
#include <vector>

namespace reverSilly::Lang::Eval {

// Bind CLI arguments as a pure immutable struct `args` in root #this:
//   args { count: raw8 (u64), 0: RawBytes, 1: RawBytes, ... }
void install_cli_args(Runtime::RuntimeContext& rt, const std::vector<std::string_view>& args);

} // namespace reverSilly::Lang::Eval
