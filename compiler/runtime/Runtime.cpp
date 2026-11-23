#include "Runtime.h"

#include <cstring>
#include <stdexcept>

namespace reverSilly::Lang::Runtime {

RuntimeContext::RuntimeContext() : symbols(), shapes(symbols) {
    Coroutine* main_coro = coroutines.create();
    main_coro->state = CoroutineState::Running;
    main_coro->push_frame();
    active_coro = main_coro;
}

void RuntimeContext::flush_pending_reclaim() {
    if (pending_reclaim_coro != 0) {
        uint64_t h = pending_reclaim_coro;
        pending_reclaim_coro = 0;
        coroutines.destroy(h);
    }
}

ConstructingFrame* RuntimeContext::current_frame() noexcept {
    return active_coro ? active_coro->current_frame() : nullptr;
}

ConstructingFrame* RuntimeContext::find_open_frame(const PointerLayout& ptr) const noexcept {
    if (ptr.is_null() || ptr.shape_ptr == 0) return nullptr;
    if (active_coro) {
        for (auto it = active_coro->frames.rbegin(); it != active_coro->frames.rend(); ++it) {
            ConstructingFrame* f = it->get();
            if (f->frame_addr() == ptr.addr &&
                reinterpret_cast<uint64_t>(&f->live_shape) == ptr.shape_ptr) {
                return f;
            }
        }
    }
    return coroutines.find_open_frame(ptr);
}

Value RuntimeContext::make_unit() const { return Value{shapes.unit_shape, {}}; }


Value RuntimeContext::make_symbol(SymbolId id) const { return Value::from_pod(shapes.symbol_shape, id); }
Value RuntimeContext::make_pointer(const PointerLayout& p) const { return Value::from_pod(shapes.pointer_shape, p); }
Value RuntimeContext::make_proto(uint64_t proto_addr) const { return Value::from_pod(shapes.prototype_shape, proto_addr); }
Value RuntimeContext::make_handle(uint64_t handle_val) const { return Value::from_pod(shapes.handle_shape, handle_val); }

Value RuntimeContext::make_raw_bytes(std::string_view data) {
    return make_raw_bytes(data.data(), data.size());
}

Value RuntimeContext::make_raw_bytes(const void* data, uint64_t len) {
    uint64_t leaf = shapes.intern_leaf(len, 1);
    Value out;
    out.shape_addr = leaf;
    if (len > 0) {
        const auto* ptr = static_cast<const uint8_t*>(data);
        out.bytes.assign(ptr, ptr + len);
    }
    return out;
}

PointerLayout RuntimeContext::field(const PointerLayout& ptr, SymbolId sym) const noexcept {
    return shapes.field_of(ptr, sym);
}

Value RuntimeContext::deref(const PointerLayout& ptr) const {
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

void RuntimeContext::write(const PointerLayout& dst_ptr, const Value& val) {
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

} // namespace reverSilly::Lang::Runtime
