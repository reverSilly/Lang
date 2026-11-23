#pragma once

#include "FieldMap.h"
#include "FrameStack.h"
#include "Prototype.h"
#include "Shape.h"
#include "SymbolPool.h"

#include <cstdint>

namespace reverSilly::Lang::Runtime {

class RuntimeContext {
public:
    SymbolPool symbols;
    ShapeArena shapes;
    PrototypeArena prototypes;
    CoroutinePool coroutines;
    Coroutine* active_coro = nullptr;
    uint64_t pending_reclaim_coro = 0;

    RuntimeContext();

    void flush_pending_reclaim();
    ConstructingFrame* current_frame() noexcept;

    Value make_unit() const;
    Value make_symbol(SymbolId id) const;
    Value make_pointer(const PointerLayout& p) const;
    Value make_proto(uint64_t proto_addr) const;
    Value make_handle(uint64_t handle_val) const;

    // Immutable fat Pointer to a contiguous byte buffer: Shape{ size = len, align = 1, fields = empty }.
    Value make_bytes_ptr(const void* data, uint64_t len);

    // Unified single-path #field: zero open-frame special cases.
    PointerLayout field(const PointerLayout& ptr, SymbolId sym) const noexcept;
    Value deref(const PointerLayout& ptr) const;
    void write(const PointerLayout& dst_ptr, const Value& val);
};

} // namespace reverSilly::Lang::Runtime
