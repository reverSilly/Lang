#pragma once

#include "FieldMap.h"
#include "FrameStack.h"
#include "Prototype.h"
#include "Shape.h"
#include "SymbolPool.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace reverSilly::Lang::Runtime {

// Transparent 16-byte Bytes slice layout: { ptr: 8B/8, len: 8B/8 }
struct alignas(8) BytesLayout {
    uint64_t ptr = 0;
    uint64_t len = 0;
};

static_assert(sizeof(BytesLayout) == 16, "BytesLayout must be 16 bytes");
static_assert(alignof(BytesLayout) == 8, "BytesLayout must be 8-byte aligned");

class RuntimeContext {
public:
    SymbolPool symbols;
    ShapeArena shapes;
    PrototypeArena prototypes;
    CoroutinePool coroutines;
    Coroutine* active_coro = nullptr;
    uint64_t bytes_shape = 0;

    uint64_t pending_reclaim_coro = 0;

    RuntimeContext() : symbols(), shapes(symbols) {
        bytes_shape = shapes.intern_struct_auto({
            {symbols.intern("ptr"), shapes.raw8_shape, 0},
            {symbols.intern("len"), shapes.raw8_shape, 0},
        });

        Coroutine* main_coro = coroutines.create();
        main_coro->state = CoroutineState::Running;
        main_coro->push_frame();
        active_coro = main_coro;
    }

    void flush_pending_reclaim() {
        if (pending_reclaim_coro != 0) {
            uint64_t h = pending_reclaim_coro;
            pending_reclaim_coro = 0;
            coroutines.destroy(h);
        }
    }

    ConstructingFrame* current_frame() noexcept {
        return active_coro ? active_coro->current_frame() : nullptr;
    }

    Value make_unit() const { return Value{shapes.unit_shape, {}}; }

    Value make_bool(bool b) const {
        uint8_t v = b ? 1 : 0;
        return Value::from_pod(shapes.boolean_shape, v);
    }

    Value make_symbol(SymbolId id) const { return Value::from_pod(shapes.symbol_shape, id); }
    Value make_pointer(const PointerLayout& p) const { return Value::from_pod(shapes.pointer_shape, p); }
    Value make_proto(uint64_t proto_addr) const { return Value::from_pod(shapes.prototype_shape, proto_addr); }
    Value make_handle(uint64_t handle_val) const { return Value::from_pod(shapes.handle_shape, handle_val); }
    Value make_bytes(const BytesLayout& b) const { return Value::from_pod(bytes_shape, b); }

    // Unified single-path #field: zero open-frame special cases!
    PointerLayout field(const PointerLayout& ptr, SymbolId sym) const noexcept {
        return shapes.field_of(ptr, sym);
    }

    Value deref(const PointerLayout& ptr) const {
        if (ptr.is_null() || ptr.shape_ptr == 0) {
            throw std::runtime_error("#deref: null or untyped pointer");
        }
        ConstructingFrame* cur = active_coro ? active_coro->current_frame() : nullptr;
        while (cur) {
            if (FrameSlot* slot = cur->find_direct_slot(ptr.addr)) {
                if (slot->state != SlotState::Live) {
                    throw std::runtime_error("#deref: cannot read Uninitialized or Empty slot");
                }
                break;
            }
            cur = cur->parent;
        }

        const auto* s = reinterpret_cast<const ShapeLayout*>(ptr.shape_ptr);
        Value out;
        out.shape_addr = ptr.shape_ptr;
        out.bytes.resize(s->size);
        if (s->size > 0) {
            std::memcpy(out.bytes.data(), reinterpret_cast<const void*>(ptr.addr), s->size);
        }
        return out;
    }

    void write(const PointerLayout& dst_ptr, const Value& val) {
        if (dst_ptr.is_null() || dst_ptr.shape_ptr == 0) {
            throw std::runtime_error("#write: null destination pointer");
        }
        if (dst_ptr.shape_ptr != val.shape_addr) {
            throw std::runtime_error("#write: shape mismatch between destination and value");
        }

        ConstructingFrame* cur = active_coro ? active_coro->current_frame() : nullptr;
        FrameSlot* direct_slot = nullptr;
        while (cur) {
            direct_slot = cur->find_direct_slot(dst_ptr.addr);
            if (direct_slot) break;
            cur = cur->parent;
        }

        if (direct_slot) {
            bool allowed = (direct_slot->state == SlotState::Uninitialized) ||
                           (dst_ptr.mutability == 1 && direct_slot->mutability == 1);
            if (!allowed) {
                throw std::runtime_error("#write: cannot overwrite immutable live/empty slot");
            }
            if (!val.bytes.empty()) {
                std::memcpy(direct_slot->storage.data(), val.bytes.data(), val.bytes.size());
            }
            direct_slot->state = SlotState::Live;
            cur->sync_live_shape();
            return;
        }

        if (dst_ptr.mutability != 1) {
            throw std::runtime_error("#write: cannot write through immutable pointer");
        }
        if (!val.bytes.empty()) {
            std::memcpy(reinterpret_cast<void*>(dst_ptr.addr), val.bytes.data(), val.bytes.size());
        }
    }
};

} // namespace reverSilly::Lang::Runtime
