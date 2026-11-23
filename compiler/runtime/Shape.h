#pragma once

#include "FieldMap.h"
#include "SymbolPool.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Runtime {

// Unified transparent 40-byte Shape layout serving BOTH closed Shapes and open ConstructingFrames.
// Fields: { size: 8B/8, align: 8B/8, fields: FieldMapInline (24B/8), __shape_tag: {} }
struct alignas(8) ShapeLayout {
    uint64_t size = 0;
    uint64_t align = 1;
    FieldMapInline fields;

    bool is_leaf() const noexcept { return fields.is_empty(); }
};

static_assert(sizeof(ShapeLayout) == 40, "ShapeLayout must be 40 bytes");
static_assert(alignof(ShapeLayout) == 8, "ShapeLayout must be 8-byte aligned");

// Runtime boxed value: shape pointer + exact raw bytes matching shape->size.
struct Value {
    uint64_t shape_addr = 0;
    std::vector<uint8_t> bytes;

    const ShapeLayout* shape() const noexcept {
        return reinterpret_cast<const ShapeLayout*>(shape_addr);
    }

    template <typename T>
    static Value from_pod(uint64_t s_addr, const T& v) {
        Value out;
        out.shape_addr = s_addr;
        out.bytes.resize(sizeof(T));
        std::memcpy(out.bytes.data(), &v, sizeof(T));
        return out;
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

struct ShapeKey {
    uint64_t size = 0;
    uint64_t align = 1;
    std::vector<FieldEntry> entries;

    bool operator==(const ShapeKey& o) const noexcept;
};

struct ShapeKeyHash {
    size_t operator()(const ShapeKey& k) const noexcept;
};

uint64_t align_up(uint64_t value, uint64_t alignment) noexcept;

class ShapeArena {
private:
    std::vector<std::unique_ptr<ShapeLayout>> shapes_;
    std::unordered_map<ShapeKey, uint64_t, ShapeKeyHash> intern_table_;

    void bootstrap(SymbolPool& syms);

public:
    FieldMapArena field_maps;

    // Bootstrapped leaf Shapes
    uint64_t unit_shape = 0;  // 0B / align 1
    uint64_t raw1_shape = 0;  // 1B / align 1
    uint64_t raw2_shape = 0;  // 2B / align 2
    uint64_t raw4_shape = 0;  // 4B / align 4
    uint64_t raw8_shape = 0;  // 8B / align 8

    // Bootstrapped builtin transparent Shapes
    uint64_t field_map_shape = 0;   // 24B/8
    uint64_t shape_meta = 0;        // 40B/8
    uint64_t symbol_shape = 0;      // 4B/4
    uint64_t pointer_shape = 0;     // 24B/8
    uint64_t field_entry_shape = 0; // { sym: Symbol @0, state: 1B @4, ptr: Pointer @8 } (32B/8)
    uint64_t prototype_shape = 0;   // 8B/8
    uint64_t handle_shape = 0;      // 8B/8

    struct FieldSpec {
        SymbolId sym;
        uint64_t sub_shape_addr;
        uint8_t mutability;
    };

    explicit ShapeArena(SymbolPool& syms);

    uint64_t intern_leaf(uint64_t size, uint64_t align);
    uint64_t intern_struct(const std::vector<FieldEntry>& entries, uint64_t size, uint64_t align);
    uint64_t intern_struct_auto(const std::vector<FieldSpec>& specs);

    // Unified single-path #field(ptr, sym): works identically on open frames, closed objects, and addr=0 z_ptr.
    PointerLayout field_of(const PointerLayout& ptr, SymbolId sym) const noexcept;

    size_t interned_count() const noexcept { return shapes_.size(); }
};

} // namespace reverSilly::Lang::Runtime
