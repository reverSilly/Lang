#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace reverSilly::Lang::Ast {

// 16-byte packed executable trailer appended at EOF:
//   [0..7]  payload_size : uint64_t little-endian (size of Ast::serialize blob)
//   [8..15] magic        : "LANGPK01"
inline constexpr std::array<uint8_t, 8> kPackTrailerMagic = {'L', 'A', 'N', 'G', 'P', 'K', '0', '1'};
inline constexpr size_t kPackTrailerSize = sizeof(uint64_t) + kPackTrailerMagic.size();

inline void encode_pack_trailer(uint64_t payload_size, std::array<uint8_t, kPackTrailerSize>& out) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((payload_size >> (i * 8)) & 0xFFu);
    }
    for (size_t i = 0; i < 8; ++i) {
        out[8 + i] = kPackTrailerMagic[i];
    }
}

inline uint64_t decode_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

} // namespace reverSilly::Lang::Ast
