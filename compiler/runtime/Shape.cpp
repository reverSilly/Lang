#include "Shape.h"

#include <algorithm>
#include <functional>

namespace reverSilly::Lang::Runtime {

bool ShapeKey::operator==(const ShapeKey& o) const noexcept {
    if (size != o.size || align != o.align || entries.size() != o.entries.size()) {
        return false;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].sym != o.entries[i].sym ||
            entries[i].state != o.entries[i].state ||
            entries[i].offset() != o.entries[i].offset() ||
            entries[i].sub_shape_addr() != o.entries[i].sub_shape_addr() ||
            entries[i].mutability() != o.entries[i].mutability()) {
            return false;
        }
    }
    return true;
}

size_t ShapeKeyHash::operator()(const ShapeKey& k) const noexcept {
    size_t h = std::hash<uint64_t>{}(k.size) ^ (std::hash<uint64_t>{}(k.align) << 1);
    for (const auto& e : k.entries) {
        size_t eh = std::hash<uint32_t>{}(e.sym) ^
                    (std::hash<uint8_t>{}(e.state) << 1) ^
                    (std::hash<uint64_t>{}(e.offset()) << 2) ^
                    (std::hash<uint64_t>{}(e.sub_shape_addr()) << 3) ^
                    (std::hash<uint8_t>{}(e.mutability()) << 4);
        h ^= eh + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

uint64_t align_up(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1) return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

ShapeArena::ShapeArena(SymbolPool& syms) {
    bootstrap(syms);
}

uint64_t ShapeArena::intern_leaf(uint64_t size, uint64_t align) {
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

uint64_t ShapeArena::intern_struct(const std::vector<FieldEntry>& entries, uint64_t size, uint64_t align) {
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

uint64_t ShapeArena::intern_struct_auto(const std::vector<FieldSpec>& specs) {
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
            static_cast<uint8_t>(SlotState::Live),
            {0, 0, 0},
            PointerLayout{offset, sp.sub_shape_addr, sp.mutability}
        });
        offset += sub->size;
    }
    uint64_t total_size = align_up(offset, max_align);
    return intern_struct(entries, total_size, max_align);
}

PointerLayout ShapeArena::freeze_bytes(uint64_t shape_addr, std::span<const uint8_t> data) {
    frozen_blobs_.emplace_back(data.begin(), data.end());
    uint64_t addr = frozen_blobs_.back().empty()
        ? reinterpret_cast<uint64_t>(&frozen_blobs_.back())
        : reinterpret_cast<uint64_t>(frozen_blobs_.back().data());
    return PointerLayout{addr, shape_addr, 0};
}

PointerLayout ShapeArena::field_of(const PointerLayout& ptr, SymbolId sym) const noexcept {
    if (ptr.shape_ptr == 0) {
        return PointerLayout{};
    }
    const auto* s = reinterpret_cast<const ShapeLayout*>(ptr.shape_ptr);
    const FieldEntry* fe = s->fields.find(sym);
    if (!fe) {
        return PointerLayout{};
    }
    uint8_t eff_mut = (ptr.mutability && fe->mutability()) ? 1 : 0;
    return PointerLayout{ptr.addr + fe->offset(), fe->sub_shape_addr(), eff_mut};
}

void ShapeArena::bootstrap(SymbolPool& syms) {
    unit_shape = intern_leaf(0, 1);
    raw1_shape = intern_leaf(1, 1);
    raw4_shape = intern_leaf(4, 4);
    raw8_shape = intern_leaf(8, 8);

    field_map_shape = intern_struct_auto({
        {syms.intern("entries_ptr"), raw8_shape, 0},
        {syms.intern("count"), raw4_shape, 0},
        {syms.intern("bucket_mask"), raw4_shape, 0},
        {syms.intern("buckets_ptr"), raw8_shape, 0},
        {syms.intern("__field_map_tag"), unit_shape, 0},
    });

    shape_meta = intern_struct_auto({
        {syms.intern("size"), raw8_shape, 0},
        {syms.intern("align"), raw8_shape, 0},
        {syms.intern("fields"), field_map_shape, 0},
        {syms.intern("__shape_tag"), unit_shape, 0},
    });

    symbol_shape = intern_struct_auto({
        {syms.intern("__raw"), raw4_shape, 0},
        {syms.intern("__sym_tag"), unit_shape, 0},
    });

    boolean_shape = intern_struct_auto({
        {syms.intern("__raw"), raw1_shape, 0},
        {syms.intern("__bool_tag"), unit_shape, 0},
    });

    pointer_shape = intern_struct_auto({
        {syms.intern("addr"), raw8_shape, 0},
        {syms.intern("shape_ptr"), raw8_shape, 0},
        {syms.intern("mutability"), raw1_shape, 0},
        {syms.intern("__ptr_tag"), unit_shape, 0},
    });

    field_entry_shape = intern_struct_auto({
        {syms.intern("sym"), symbol_shape, 0},
        {syms.intern("state"), raw1_shape, 0},
        {syms.intern("ptr"), pointer_shape, 0},
    });

    prototype_shape = intern_struct_auto({
        {syms.intern("__raw"), raw8_shape, 0},
        {syms.intern("__proto_tag"), unit_shape, 0},
    });

    handle_shape = intern_struct_auto({
        {syms.intern("__raw"), raw8_shape, 0},
        {syms.intern("__handle_tag"), unit_shape, 0},
    });
}

} // namespace reverSilly::Lang::Runtime
