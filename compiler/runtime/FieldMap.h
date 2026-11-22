#pragma once

#include "SymbolPool.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace reverSilly::Lang::Runtime {

enum class SlotState : uint8_t {
    Uninitialized = 0,
    Live = 1,
    Empty = 2,
};

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

// Unified transparent 32-byte FieldEntry layout serving BOTH closed Shapes and open ConstructingFrames:
//   @0   sym:   Symbol  (4B)  — 0 = unbound anonymous slot (open frames only)
//   @4   state: u8      (1B)  — 0 = Uninitialized, 1 = Live, 2 = Empty (closed fields always 1 = Live)
//   @5   pad:   3B
//   @8   ptr:   Pointer (24B) — relative fat-pointer { offset, sub_shape_ptr, mutability }
struct alignas(8) FieldEntry {
    SymbolId sym = 0;
    uint8_t state = static_cast<uint8_t>(SlotState::Live);
    uint8_t pad[3] = {0, 0, 0};
    PointerLayout rel_ptr;

    uint64_t offset() const noexcept { return rel_ptr.addr; }
    uint64_t sub_shape_addr() const noexcept { return rel_ptr.shape_ptr; }
    uint8_t mutability() const noexcept { return rel_ptr.mutability; }
    SlotState slot_state() const noexcept { return static_cast<SlotState>(state); }
    bool is_live_bound() const noexcept { return sym != 0 && state == static_cast<uint8_t>(SlotState::Live); }
};

static_assert(sizeof(FieldEntry) == 32, "FieldEntry must be 32 bytes");
static_assert(alignof(FieldEntry) == 8, "FieldEntry must be 8-byte aligned");
static_assert(offsetof(FieldEntry, sym) == 0, "FieldEntry::sym must be at offset 0");
static_assert(offsetof(FieldEntry, state) == 4, "FieldEntry::state must be at offset 4");
static_assert(offsetof(FieldEntry, rel_ptr) == 8, "FieldEntry::rel_ptr must be at offset 8");

uint32_t hash_symbol_id(SymbolId sym) noexcept;

// Inlined 24-byte layout embedded inside ShapeLayout.fields.
struct alignas(8) FieldMapInline {
    static constexpr uint32_t kEmptyBucket = 0xFFFFFFFFu;
    static constexpr uint32_t kLinearThreshold = 8;

    uint64_t entries_ptr = 0;
    uint32_t count = 0;
    uint32_t bucket_mask = 0;
    uint64_t buckets_ptr = 0;

    bool is_empty() const noexcept { return count == 0; }

    const FieldEntry* entries_begin() const noexcept {
        return reinterpret_cast<const FieldEntry*>(entries_ptr);
    }

    const FieldEntry& at(uint32_t idx) const;
    const FieldEntry* find(SymbolId sym) const noexcept;

    static uint32_t next_pow2(uint32_t n) noexcept;
    static FieldMapInline build_view(const std::vector<FieldEntry>& entries, std::vector<uint32_t>& buckets_buf);
};

static_assert(sizeof(FieldMapInline) == 24, "FieldMapInline must be 24 bytes");
static_assert(alignof(FieldMapInline) == 8, "FieldMapInline must be 8-byte aligned");

// Owns immutable flat FieldEntry[] & open-addressing uint32_t[] buffers for closed .rodata Shapes.
class FieldMapArena {
private:
    struct Storage {
        std::unique_ptr<FieldEntry[]> entries;
        std::unique_ptr<uint32_t[]> buckets;
    };

    std::vector<Storage> blocks_;

public:
    static FieldMapInline empty() noexcept { return FieldMapInline{}; }
    static bool is_empty(const FieldMapInline& m) noexcept { return m.is_empty(); }
    static uint32_t count(const FieldMapInline& m) noexcept { return m.count; }
    static PointerLayout get(const FieldMapInline& m, SymbolId sym) noexcept;
    static FieldEntry at(const FieldMapInline& m, uint32_t idx);

    FieldMapInline build(const std::vector<FieldEntry>& entries);
    FieldMapInline insert(const FieldMapInline& base, SymbolId sym, const PointerLayout& rel_ptr);
};

} // namespace reverSilly::Lang::Runtime
