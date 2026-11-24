#pragma once

#include <cstdint>
#include <span>

namespace reverSilly::Lang::Eval {

// Returns the compiled `lang_stub` executable image embedded inside `lang_compiler`.
std::span<const uint8_t> embedded_lang_stub();

} // namespace reverSilly::Lang::Eval
