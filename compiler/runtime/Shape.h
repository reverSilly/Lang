#pragma once

#include "FieldMap.h"
#include "SymbolPool.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Runtime {

// Transparent 40-byte Shape layout residing in .rodata.
// Fields: { size: 8B/8, align: 8B/8, fields: FieldMapInline (24B/8), __shape_tag: {} }
struct alignas(8) ShapeLayout {
    uint64_t size = 0;
    uint64_t align = 1;
    FieldMapInline fields{};

    bool is_leaf() const noexcept { return fields.is_empty(); }
};

static_assert(sizeof(ShapeLayout) == 40, "ShapeLayout must be 40 bytes");
static_assert(alignof(ShapeLayout) == 8, "ShapeLayout must be 8-byte aligned");

// Any expression result in Core is a raw bit-slice Value paired with its Shape address.
struct Value {
    uint64_t shape_addr = 0;
    std::vector<uint8_t> bytes;

    const ShapeLayout* shape() const noexcept {
        return reinterpret_cast<const ShapeLayout*>(shape_addr);
    }

    template <typename T>
    static Value from_pod(uint64_t shape_addr, const T& obj) {
        Value v;
        v.shape_addr = shape_addr;
        v.bytes.resize(sizeof(T));
        std::memcpy(v.bytes.data(), &obj, sizeof(T));
        return v;
    }

    template <typename T>
    T as_pod() const {
        if (bytes.size() != sizeof(T)) {
            throw std::runtime_error("Value::as_pod: byte size mismatch");
        }
        T out{};
        std::memcpy(&out, bytes.data(), sizeof(T));
        return out;
    }
};

// Shallow canonical key for O(fields) Shape interning (since sub_shape_addr is already interned).
struct ShapeKey {
    uint64_t size = 0;
    uint64_t align = 1;
    std::vector<FieldEntry> entries;

    bool operator==(const ShapeKey& o) const noexcept {
        if (size != o.size || align != o.align || entries.size() != o.entries.size()) {
            return false;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].sym != o.entries[i].sym ||
                entries[i].offset() != o.entries[i].offset() ||
                entries[i].sub_shape_addr() != o.entries[i].sub_shape_addr() ||
                entries[i].mutability() != o.entries[i].mutability()) {
                return false;
            }
        }
        return true;
    }
};

struct ShapeKeyHash {
    size_t operator()(const ShapeKey& k) const noexcept {
        size_t h = std::hash<uint64_t>{}(k.size) ^ (std::hash<uint64_t>{}(k.align) << 1);
        for (const auto& e : k.entries) {
            size_t eh = std::hash<uint32_t>{}(e.sym) ^
                        (std::hash<uint64_t>{}(e.offset()) << 1) ^
                        (std::hash<uint64_t>{}(e.sub_shape_addr()) << 2) ^
                        (std::hash<uint8_t>{}(e.mutability()) << 3);
            h ^= eh + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

inline uint64_t align_up(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1) return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

// Read-only .rodata arena with shallow Shape interning & 6 builtin transparent Shapes bootstrap.
class ShapeArena {
private:
    std::vector<std::unique_ptr<ShapeLayout>> shapes_;
    std::vector<std::vector<uint8_t>> frozen_blobs_;
    std::unordered_map<ShapeKey, uint64_t, ShapeKeyHash> intern_table_;

public:
    FieldMapArena field_maps;

    // Bootstrapped leaf Shapes (size/align base cases, fields = empty_map)
    uint64_t unit_shape = 0;  // 0B / align 1 (empty object {})
    uint64_t raw1_shape = 0;  // 1B / align 1
    uint64_t raw4_shape = 0;  // 4B / align 4
    uint64_t raw8_shape = 0;  // 8B / align 8

    // Bootstrapped builtin transparent Shapes
    uint64_t field_map_shape = 0;   // { entries_ptr, count, bucket_mask, buckets_ptr, __field_map_tag } (24B/8)
    uint64_t shape_meta = 0;        // { size, align, fields, __shape_tag } (40B/8)
    uint64_t symbol_shape = 0;      // { __raw: 4B/4, __sym_tag: {} } (4B/4)
    uint64_t boolean_shape = 0;     // { __raw: 1B/1, __bool_tag: {} } (1B/1)
    uint64_t pointer_shape = 0;     // { addr: 8B/8, shape_ptr: 8B/8, mutability: 1B/1, __ptr_tag: {} } (24B/8)
    uint64_t field_entry_shape = 0; // { sym: Symbol, ptr: Pointer } (32B/8)
    uint64_t prototype_shape = 0;   // { __raw: 8B/8, __proto_tag: {} } (8B/8)
    uint64_t handle_shape = 0;      // { __raw: 8B/8, __handle_tag: {} } (8B/8)

    explicit ShapeArena(SymbolPool& syms) {
        bootstrap(syms);
    }

    uint64_t intern_leaf(uint64_t size, uint64_t align) {
        if (align == 0 || (align & (align - 1)) != 0) {
            throw std::invalid_argument("ShapeArena::intern_leaf: align must be a power of 2");
        }
        ShapeKey key{size, align, {}};
        auto it = intern_table_.find(key);
        if (it != intern_table_.end()) {
            return it->second;
        }
        auto s = std::make_unique<ShapeLayout>();
        s->size = size;
        s->align = align;
        s->fields = FieldMapArena::empty();
        uint64_t addr = reinterpret_cast<uint64_t>(s.get());
        shapes_.push_back(std::move(s));
        intern_table_.emplace(std::move(key), addr);
        return addr;
    }

    uint64_t intern_struct(const std::vector<FieldEntry>& entries, uint64_t size, uint64_t align) {
        ShapeKey key{size, align, entries};
        auto it = intern_table_.find(key);
        if (it != intern_table_.end()) {
            return it->second;
        }
        auto s = std::make_unique<ShapeLayout>();
        s->size = size;
        s->align = align;
        s->fields = field_maps.build(entries);
        uint64_t addr = reinterpret_cast<uint64_t>(s.get());
        shapes_.push_back(std::move(s));
        intern_table_.emplace(std::move(key), addr);
        return addr;
    }

    struct FieldSpec {
        SymbolId sym;
        uint64_t sub_shape_addr;
        uint8_t mutability;
    };

    uint64_t intern_struct_auto(const std::vector<FieldSpec>& specs) {
        uint64_t max_align = 1;
        uint64_t offset = 0;
        std::vector<FieldEntry> entries;
        entries.reserve(specs.size());

        for (const auto& sp : specs) {
            const auto* sub = reinterpret_cast<const ShapeLayout*>(sp.sub_shape_addr);
            uint64_t f_align = std::max<uint64_t>(1, sub->align);
            max_align = std::max(max_align, f_align);
            offset = align_up(offset, f_align);
            entries.push_back(FieldEntry{
                sp.sym,
                0,
                PointerLayout{offset, sp.sub_shape_addr, sp.mutability}
            });
            offset += sub->size;
        }
        uint64_t total_size = align_up(offset, max_align);
        return intern_struct(entries, total_size, max_align);
    }

    // Freeze arbitrary bytes into .rodata and return an immutable PointerLayout.
    PointerLayout freeze_bytes(uint64_t shape_addr, std::span<const uint8_t> data) {
        frozen_blobs_.emplace_back(data.begin(), data.end());
        uint64_t addr = frozen_blobs_.back().empty()
            ? reinterpret_cast<uint64_t>(&frozen_blobs_.back())
            : reinterpret_cast<uint64_t>(frozen_blobs_.back().data());
        return PointerLayout{addr, shape_addr, 0};
    }

    // Perform #field(ptr, sym) on a closed/live PointerLayout (also supports addr=0 z_ptr reflection).
    PointerLayout field_of(const PointerLayout& ptr, SymbolId sym) const noexcept {
        if (ptr.shape_ptr == 0) {
            return PointerLayout{}; // null
        }
        const auto* s = reinterpret_cast<const ShapeLayout*>(ptr.shape_ptr);
        const FieldEntry* fe = s->fields.find(sym);
        if (!fe) {
            return PointerLayout{}; // null
        }
        uint8_t eff_mut = (ptr.mutability && fe->mutability()) ? 1 : 0;
        return PointerLayout{ptr.addr + fe->offset(), fe->sub_shape_addr(), eff_mut};
    }

    size_t interned_count() const noexcept { return shapes_.size(); }

private:
    void bootstrap(SymbolPool& syms) {
        // 1. Base leaf shapes
        unit_shape = intern_leaf(0, 1);
        raw1_shape = intern_leaf(1, 1);
        raw4_shape = intern_leaf(4, 4);
        raw8_shape = intern_leaf(8, 8);

        // 2. FieldMapInline shape (24B/8): { entries_ptr: 8B, count: 4B, bucket_mask: 4B, buckets_ptr: 8B }
        field_map_shape = intern_struct_auto({
            {syms.intern("entries_ptr"), raw8_shape, 0},
            {syms.intern("count"), raw4_shape, 0},
            {syms.intern("bucket_mask"), raw4_shape, 0},
            {syms.intern("buckets_ptr"), raw8_shape, 0},
            {syms.intern("__field_map_tag"), unit_shape, 0},
        });

        // 3. ShapeMeta (40B/8) — self-describing fixed point
        shape_meta = intern_struct_auto({
            {syms.intern("size"), raw8_shape, 0},
            {syms.intern("align"), raw8_shape, 0},
            {syms.intern("fields"), field_map_shape, 0},
            {syms.intern("__shape_tag"), unit_shape, 0},
        });

        // 4. Symbol (4B/4)
        symbol_shape = intern_struct_auto({
            {syms.intern("__raw"), raw4_shape, 0},
            {syms.intern("__sym_tag"), unit_shape, 0},
        });

        // 5. Boolean (1B/1)
        boolean_shape = intern_struct_auto({
            {syms.intern("__raw"), raw1_shape, 0},
            {syms.intern("__bool_tag"), unit_shape, 0},
        });

        // 6. Pointer (24B/8)
        pointer_shape = intern_struct_auto({
            {syms.intern("addr"), raw8_shape, 0},
            {syms.intern("shape_ptr"), raw8_shape, 0},
            {syms.intern("mutability"), raw1_shape, 0},
            {syms.intern("__ptr_tag"), unit_shape, 0},
        });

        // 7. FieldEntry (32B/8): transparent { sym: Symbol, ptr: Pointer }
        field_entry_shape = intern_struct_auto({
            {syms.intern("sym"), symbol_shape, 0},
            {syms.intern("ptr"), pointer_shape, 0},
        });

        // 8. Prototype (8B/8)
        prototype_shape = intern_struct_auto({
            {syms.intern("__raw"), raw8_shape, 0},
            {syms.intern("__proto_tag"), unit_shape, 0},
        });

        // 9. Handle (8B/8)
        handle_shape = intern_struct_auto({
            {syms.intern("__raw"), raw8_shape, 0},
            {syms.intern("__handle_tag"), unit_shape, 0},
        });
    }
};

} // namespace reverSilly::Lang::Runtime
