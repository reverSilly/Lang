#pragma once

#include "Trie.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace reverSilly::Lang::Runtime {

using SymbolId = uint32_t;

// Per-Runtime-instance symbol interning pool backed by Trie.
// Static Words in source code are interned at parse time into 4-byte SymbolIds;
// dynamic text <-> Symbol conversions are exposed via capabilities.symbols.
class SymbolPool {
private:
    Trie<SymbolId> trie_;
    std::vector<std::string> names_;

public:
    SymbolPool() {
        // Reserve ID 0 as invalid/sentinel internally (user symbols start at 1).
        names_.emplace_back("");
    }

    SymbolId intern(std::span<const uint8_t> bytes) {
        return trie_.get_or_emplace(bytes, [&]() -> SymbolId {
            SymbolId id = static_cast<SymbolId>(names_.size());
            names_.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return id;
        });
    }

    SymbolId intern(std::string_view text) {
        auto ptr = reinterpret_cast<const uint8_t*>(text.data());
        return intern(std::span<const uint8_t>(ptr, text.size()));
    }

    std::string_view name(SymbolId id) const {
        if (id == 0 || id >= names_.size()) {
            throw std::out_of_range("SymbolPool::name: invalid SymbolId");
        }
        return names_[id];
    }

    size_t size() const noexcept {
        return names_.size() - 1;
    }
};

} // namespace reverSilly::Lang::Runtime
