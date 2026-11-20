#pragma once

#include "SymbolPool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace reverSilly::Lang::Runtime {

// Transparent 24-byte Pointer layout.
// Fields: { addr: 8B/8, shape_ptr: 8B/8, mutability: 1B/1, __ptr_tag: {} }
struct alignas(8) PointerLayout {
    uint64_t addr = 0;
    uint64_t shape_ptr = 0;
    uint8_t mutability = 0; // 0 = immutable, 1 = mutable
    uint8_t pad[7] = {0, 0, 0, 0, 0, 0, 0};

    bool is_null() const noexcept { return addr == 0 && shape_ptr == 0; }
};

static_assert(sizeof(PointerLayout) == 24, "PointerLayout must be 24 bytes");
static_assert(alignof(PointerLayout) == 8, "PointerLayout must be 8-byte aligned");

// Transparent 32-byte FieldEntry layout matching Lang struct { sym: Symbol, ptr: Pointer }.
// rel_ptr is a relative fat-pointer with addr = field byte offset from base 0.
struct alignas(8) FieldEntry {
    SymbolId sym = 0;      // 4B (Symbol)
    uint32_t pad = 0;      // 4B alignment padding for PointerLayout
    PointerLayout rel_ptr; // 24B (Pointer: { offset, sub_shape_ptr, mutability })

    uint64_t offset() const noexcept { return rel_ptr.addr; }
    uint64_t sub_shape_addr() const noexcept { return rel_ptr.shape_ptr; }
    uint8_t mutability() const noexcept { return rel_ptr.mutability; }
};

static_assert(sizeof(FieldEntry) == 32, "FieldEntry must be 32 bytes");
static_assert(alignof(FieldEntry) == 8, "FieldEntry must be 8-byte aligned");
static_assert(offsetof(FieldEntry, sym) == 0, "FieldEntry::sym must be at offset 0");
static_assert(offsetof(FieldEntry, rel_ptr) == 8, "FieldEntry::rel_ptr must be at offset 8");

inline uint32_t hash_symbol_id(SymbolId sym) noexcept {
    // Murmur3 32-bit finalizer: fast, well-distributed for sequential SymbolIds.
    uint32_t h = sym;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Inlined layout embedded directly inside ShapeLayout.fields (24 bytes, align 8).
// Flat open-addressing hash table: zero STL overhead, LLVM .rodata-serializable.
struct alignas(8) FieldMapInline {
    static constexpr uint32_t kEmptyBucket = 0xFFFFFFFFu;

    uint64_t entries_ptr = 0; // pointer to fixed-length read-only FieldEntry[count] (0 if empty)
    uint32_t count = 0;       // number of fields (0 => empty_map / leaf Shape)
    uint32_t bucket_mask = 0; // (bucket_count - 1), power-of-2; 0 => small linear scan
    uint64_t buckets_ptr = 0; // pointer to uint32_t[bucket_mask + 1] (0 if small or empty)

    bool is_empty() const noexcept { return count == 0; }

    const FieldEntry* entries_begin() const noexcept {
        return reinterpret_cast<const FieldEntry*>(entries_ptr);
    }

    const FieldEntry& at(uint32_t idx) const {
        if (idx >= count) {
            throw std::out_of_range("FieldMapInline::at: index out of range");
        }
        return entries_begin()[idx];
    }

    const FieldEntry* find(SymbolId sym) const noexcept {
        if (count == 0) {
            return nullptr;
        }
        const FieldEntry* arr = entries_begin();
        // Small structs (count <= 8): linear scan beats hashing with zero bucket indirection.
        if (buckets_ptr == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                if (arr[i].sym == sym) return &arr[i];
            }
            return nullptr;
        }
        const auto* buckets = reinterpret_cast<const uint32_t*>(buckets_ptr);
        uint32_t slot = hash_symbol_id(sym) & bucket_mask;
        while (true) {
            uint32_t idx = buckets[slot];
            if (idx == kEmptyBucket) return nullptr;
            if (arr[idx].sym == sym) return &arr[idx];
            slot = (slot + 1) & bucket_mask;
        }
    }
};

static_assert(sizeof(FieldMapInline) == 24, "FieldMapInline must be 24 bytes");
static_assert(alignof(FieldMapInline) == 8, "FieldMapInline must be 8-byte aligned");

// Owns immutable flat FieldEntry[] & open-addressing uint32_t[] buffers.
// Directly backs all capabilities.field_map operations: empty, is_empty, count, get, at, insert.
class FieldMapArena {
private:
    static constexpr uint32_t kLinearThreshold = 8;

    struct Storage {
        std::unique_ptr<FieldEntry[]> entries;
        std::unique_ptr<uint32_t[]> buckets;
    };

    std::vector<Storage> blocks_;

    static uint32_t next_pow2(uint32_t n) noexcept {
        uint32_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

public:
    static FieldMapInline empty() noexcept {
        return FieldMapInline{};
    }

    static bool is_empty(const FieldMapInline& m) noexcept {
        return m.is_empty();
    }

    static uint32_t count(const FieldMapInline& m) noexcept {
        return m.count;
    }

    // capabilities.field_map.get(map, sym) -> relative PointerLayout (null if absent)
    static PointerLayout get(const FieldMapInline& m, SymbolId sym) noexcept {
        const FieldEntry* fe = m.find(sym);
        return fe ? fe->rel_ptr : PointerLayout{};
    }

    // capabilities.field_map.at(map, idx) -> FieldEntry { sym, rel_ptr }
    static FieldEntry at(const FieldMapInline& m, uint32_t idx) {
        return m.at(idx);
    }

    // Build an immutable FieldMapInline from an ordered list of FieldEntries.
    FieldMapInline build(const std::vector<FieldEntry>& entries) {
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

        if (n > kLinearThreshold) {
            uint32_t cap = next_pow2(n * 2); // load factor <= 0.5
            uint32_t mask = cap - 1;
            st.buckets = std::make_unique<uint32_t[]>(cap);
            std::fill_n(st.buckets.get(), cap, FieldMapInline::kEmptyBucket);

            for (uint32_t i = 0; i < n; ++i) {
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

    // capabilities.field_map.insert(map, sym, rel_ptr) -> new immutable FieldMapInline
    // (Preserves insertion order; replaces in-place if sym already exists.)
    FieldMapInline insert(const FieldMapInline& base, SymbolId sym, const PointerLayout& rel_ptr) {
        std::vector<FieldEntry> next;
        next.reserve(base.count + 1);
        bool replaced = false;
        const FieldEntry* old = base.entries_begin();
        for (uint32_t i = 0; i < base.count; ++i) {
            if (old[i].sym == sym) {
                next.push_back(FieldEntry{sym, 0, rel_ptr});
                replaced = true;
            } else {
                next.push_back(old[i]);
            }
        }
        if (!replaced) {
            next.push_back(FieldEntry{sym, 0, rel_ptr});
        }
        return build(next);
    }
};

} // namespace reverSilly::Lang::Runtime
