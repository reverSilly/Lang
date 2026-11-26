#include "FieldMap.h"

#include <algorithm>
#include <stdexcept>

namespace reverSilly::Lang::Runtime {

uint32_t hash_symbol_id(SymbolId sym) noexcept {
    uint32_t h = sym;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

const FieldEntry& FieldMapInline::at(uint32_t idx) const {
    if (idx >= count) {
        throw std::out_of_range("FieldMapInline::at: index out of range");
    }
    return entries_begin()[idx];
}

const FieldEntry* FieldMapInline::find(SymbolId sym) const noexcept {
    if (count == 0 || sym == 0) {
        return nullptr;
    }
    const FieldEntry* arr = entries_begin();
    if (buckets_ptr == 0) {
        for (uint32_t i = 0; i < count; ++i) {
            if (arr[i].sym == sym) {
                return &arr[i];
            }
        }
        return nullptr;
    }
    const auto* buckets = reinterpret_cast<const uint32_t*>(buckets_ptr);
    uint32_t slot = hash_symbol_id(sym) & bucket_mask;
    while (true) {
        uint32_t idx = buckets[slot];
        if (idx == kEmptyBucket) return nullptr;
        if (arr[idx].sym == sym) {
            return &arr[idx];
        }
        slot = (slot + 1) & bucket_mask;
    }
}

uint32_t FieldMapInline::next_pow2(uint32_t n) noexcept {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

FieldMapInline FieldMapInline::build_view(const std::vector<FieldEntry>& entries, std::vector<uint32_t>& buckets_buf) {
    FieldMapInline out{};
    buckets_buf.clear();
    if (entries.empty()) {
        return out;
    }
    uint32_t n = static_cast<uint32_t>(entries.size());
    out.entries_ptr = reinterpret_cast<uint64_t>(entries.data());
    out.count = n;

    if (n > kLinearThreshold) {
        uint32_t cap = next_pow2(n * 2);
        uint32_t mask = cap - 1;
        buckets_buf.assign(cap, kEmptyBucket);
        for (uint32_t i = 0; i < n; ++i) {
            if (!entries[i].is_bound()) continue;
            uint32_t slot = hash_symbol_id(entries[i].sym) & mask;
            while (buckets_buf[slot] != kEmptyBucket) {
                slot = (slot + 1) & mask;
            }
            buckets_buf[slot] = i;
        }
        out.bucket_mask = mask;
        out.buckets_ptr = reinterpret_cast<uint64_t>(buckets_buf.data());
    }
    return out;
}

PointerLayout FieldMapArena::get(const FieldMapInline& m, SymbolId sym) noexcept {
    const FieldEntry* fe = m.find(sym);
    return fe ? fe->rel_ptr : PointerLayout{};
}

FieldEntry FieldMapArena::at(const FieldMapInline& m, uint32_t idx) {
    return m.at(idx);
}

FieldMapInline FieldMapArena::build(const std::vector<FieldEntry>& entries) {
    if (entries.empty()) {
        return FieldMapInline{};
    }
    uint32_t n = static_cast<uint32_t>(entries.size());
    Storage st;
    st.entries = std::make_unique<FieldEntry[]>(n);
    std::copy(entries.begin(), entries.end(), st.entries.get());

    FieldMapInline out;
    out.entries_ptr = reinterpret_cast<uint64_t>(st.entries.get());
    out.count = n;

    if (n > FieldMapInline::kLinearThreshold) {
        uint32_t cap = FieldMapInline::next_pow2(n * 2);
        uint32_t mask = cap - 1;
        st.buckets = std::make_unique<uint32_t[]>(cap);
        std::fill_n(st.buckets.get(), cap, FieldMapInline::kEmptyBucket);

        for (uint32_t i = 0; i < n; ++i) {
            if (!entries[i].is_bound()) continue;
            uint32_t slot = hash_symbol_id(entries[i].sym) & mask;
            while (st.buckets[slot] != FieldMapInline::kEmptyBucket) {
                slot = (slot + 1) & mask;
            }
            st.buckets[slot] = i;
        }
        out.bucket_mask = mask;
        out.buckets_ptr = reinterpret_cast<uint64_t>(st.buckets.get());
    }

    blocks_.push_back(std::move(st));
    return out;
}

FieldMapInline FieldMapArena::insert(const FieldMapInline& base, SymbolId sym, const PointerLayout& rel_ptr) {
    std::vector<FieldEntry> next;
    next.reserve(base.count + 1);
    bool replaced = false;
    const FieldEntry* old = base.entries_begin();
    for (uint32_t i = 0; i < base.count; ++i) {
        if (old[i].sym == sym) {
            next.push_back(FieldEntry{sym, static_cast<uint8_t>(SlotState::Live), {0, 0, 0}, rel_ptr});
            replaced = true;
        } else {
            next.push_back(old[i]);
        }
    }
    if (!replaced) {
        next.push_back(FieldEntry{sym, static_cast<uint8_t>(SlotState::Live), {0, 0, 0}, rel_ptr});
    }
    return build(next);
}

} // namespace reverSilly::Lang::Runtime
