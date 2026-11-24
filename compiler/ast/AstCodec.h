#pragma once

#include "Ast.h"

#include <cstdint>
#include <span>
#include <vector>

namespace reverSilly::Lang::Ast {

inline constexpr uint32_t kAstFormatVersion = 1;

// Serialize a standalone Ast::ExprPtr DAG into a portable binary blob:
//   [8-byte magic "LANGAST\0"][4-byte little-endian version][cereal PortableBinaryArchive payload]
std::vector<uint8_t> serialize(const ExprPtr& root);

// Deserialize a binary blob produced by Ast::serialize back into an equivalent ExprPtr DAG,
// preserving shared_ptr DAG identity. Throws std::runtime_error on invalid or corrupted input.
ExprPtr deserialize(std::span<const uint8_t> bytes);

} // namespace reverSilly::Lang::Ast
