#include "AstCodec.h"

#include <boost/pfr/core.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/vector.hpp>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace reverSilly::Lang::Ast {

// Scoped inside reverSilly::Lang::Ast so ADL matches only AST aggregate structs
// and never collides with STL or cereal internal aggregates.
template <class Archive, typename T>
    requires std::is_aggregate_v<T>
void serialize(Archive& ar, T& v) {
    boost::pfr::for_each_field(v, [&](auto& field) { ar(field); });
}

template <class Archive>
void serialize(Archive& ar, AstValue& v) {
    ar(v.data);
}

template <class Archive>
void save(Archive& ar, const Expr& e) {
    ar(e.node);
}

} // namespace reverSilly::Lang::Ast

namespace cereal {

template <>
struct LoadAndConstruct<reverSilly::Lang::Ast::Expr> {
    template <class Archive>
    static void load_and_construct(
        Archive& ar,
        cereal::construct<reverSilly::Lang::Ast::Expr>& construct) {
        reverSilly::Lang::Ast::Expr::Variant node;
        ar(node);
        construct(std::move(node));
    }
};

} // namespace cereal

namespace reverSilly::Lang::Ast {

namespace {

constexpr std::array<uint8_t, 8> kMagic = {'L', 'A', 'N', 'G', 'A', 'S', 'T', '\0'};
constexpr size_t kHeaderSize = kMagic.size() + sizeof(uint32_t);

void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

std::vector<uint8_t> serialize(const ExprPtr& root) {
    if (!root) {
        throw std::runtime_error("Ast::serialize: null root expression");
    }

    std::ostringstream oss(std::ios::binary);
    {
        cereal::PortableBinaryOutputArchive ar(oss);
        ar(root);
    }

    const std::string payload = oss.str();
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    write_u32_le(out, kAstFormatVersion);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

ExprPtr deserialize(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        throw std::runtime_error("Ast::deserialize: input shorter than 12-byte header");
    }
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
        throw std::runtime_error("Ast::deserialize: invalid magic header");
    }

    const uint32_t version = read_u32_le(bytes.data() + kMagic.size());
    if (version != kAstFormatVersion) {
        throw std::runtime_error(
            "Ast::deserialize: unsupported format version " + std::to_string(version)
            + " (expected " + std::to_string(kAstFormatVersion) + ")");
    }

    std::string payload(
        reinterpret_cast<const char*>(bytes.data() + kHeaderSize),
        bytes.size() - kHeaderSize);
    std::istringstream iss(payload, std::ios::binary);

    ExprPtr root;
    try {
        cereal::PortableBinaryInputArchive ar(iss);
        ar(root);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("Ast::deserialize: failed to decode payload: ") + ex.what());
    }

    if (iss.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Ast::deserialize: unexpected trailing bytes after payload");
    }
    if (!root) {
        throw std::runtime_error("Ast::deserialize: decoded root expression is null");
    }

    return root;
}

} // namespace reverSilly::Lang::Ast
