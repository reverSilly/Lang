#pragma once

#include "FieldMap.h"
#include "FrameStack.h"
#include "Shape.h"
#include "SymbolPool.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace reverSilly::Lang::Runtime {

// Thin aggregate context owning per-process SymbolPool, ShapeArena, and Coroutines.
class RuntimeContext {
public:
    SymbolPool symbols;
    ShapeArena shapes;
    std::vector<std::unique_ptr<Coroutine>> coroutines;
    Coroutine* active_coro = nullptr;

    RuntimeContext() : symbols(), shapes(symbols) {
        // Initialize main coroutine with a root constructing frame.
        auto main_coro = std::make_unique<Coroutine>();
        main_coro->state = CoroutineState::Running;
        main_coro->push_frame();
        active_coro = main_coro.get();
        coroutines.push_back(std::move(main_coro));
    }

    ConstructingFrame* current_frame() noexcept {
        return active_coro ? active_coro->current_frame() : nullptr;
    }

    // Core #deref(ptr) -> Value (POD/RAII-agnostic raw bit read)
    Value deref(const PointerLayout& ptr) const {
        if (ptr.is_null() || ptr.shape_ptr == 0) {
            throw std::runtime_error("#deref: null or untyped pointer");
        }
        // Reject reading Uninitialized or Empty direct slots in active chain.
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

    // Core #write(dst_ptr, val) (POD/RAII-agnostic raw bit write)
    void write(const PointerLayout& dst_ptr, const Value& val) {
        if (dst_ptr.is_null() || dst_ptr.shape_ptr == 0) {
            throw std::runtime_error("#write: null destination pointer");
        }
        if (dst_ptr.shape_ptr != val.shape_addr) {
            throw std::runtime_error("#write: shape mismatch between destination and value");
        }

        // Check if dst_ptr is a direct child slot in the active construction chain.
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
            return;
        }

        // Non-direct slot (e.g. mutable field inside a closed object or recast buffer):
        if (dst_ptr.mutability != 1) {
            throw std::runtime_error("#write: cannot write through immutable pointer");
        }
        if (!val.bytes.empty()) {
            std::memcpy(reinterpret_cast<void*>(dst_ptr.addr), val.bytes.data(), val.bytes.size());
        }
    }
};

} // namespace reverSilly::Lang::Runtime
