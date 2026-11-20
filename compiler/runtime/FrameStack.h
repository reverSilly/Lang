#pragma once

#include "Shape.h"
#include "SymbolPool.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Runtime {

enum class SlotState : uint8_t {
    Uninitialized,
    Live,
    Empty,
};

struct ConstructingFrame;

// A direct child location slot owned by a ConstructingFrame.
// Its Shape and storage buffer are fixed at creation time so its pointer is always a real Pointer.
struct FrameSlot {
    ConstructingFrame* owner = nullptr;
    uint64_t shape_addr = 0;
    uint8_t mutability = 0; // 0 = immutable, 1 = mutable
    SlotState state = SlotState::Uninitialized;
    bool is_bound = false;
    SymbolId bound_sym = 0;
    std::vector<uint8_t> storage;

    // Non-zero stable address (even for zero-sized types, use &storage address).
    uint64_t data_addr() const noexcept {
        if (storage.empty()) {
            return reinterpret_cast<uint64_t>(&storage);
        }
        return reinterpret_cast<uint64_t>(storage.data());
    }

    PointerLayout pointer() const noexcept {
        return PointerLayout{data_addr(), shape_addr, mutability};
    }
};

// Active constructing block frame on a coroutine's construction stack.
struct ConstructingFrame {
    ConstructingFrame* parent = nullptr;
    std::vector<std::unique_ptr<FrameSlot>> slots;
    std::unordered_map<uint64_t, FrameSlot*> addr_to_slot;
    std::unordered_map<SymbolId, FrameSlot*> sym_to_slot;
    std::vector<SymbolId> bind_order; // preserves definition order for closing flatten

    // #allocate(shape_addr, is_mut) -> real Pointer (state = Uninitialized)
    FrameSlot* allocate(uint64_t shape_addr, bool is_mut) {
        const auto* s = reinterpret_cast<const ShapeLayout*>(shape_addr);
        if (!s) {
            throw std::invalid_argument("ConstructingFrame::allocate: null shape");
        }
        auto slot = std::make_unique<FrameSlot>();
        slot->owner = this;
        slot->shape_addr = shape_addr;
        slot->mutability = is_mut ? 1 : 0;
        slot->state = SlotState::Uninitialized;
        slot->storage.resize(s->size, 0);

        FrameSlot* raw = slot.get();
        addr_to_slot.emplace(raw->data_addr(), raw);
        slots.push_back(std::move(slot));
        return raw;
    }

    // #allocate_val(val, is_mut) -> real Pointer (state = Live, sits in #unbound)
    FrameSlot* allocate_val(const Value& val, bool is_mut) {
        FrameSlot* slot = allocate(val.shape_addr, is_mut);
        if (val.bytes.size() != slot->storage.size()) {
            throw std::runtime_error("ConstructingFrame::allocate_val: size mismatch");
        }
        if (!val.bytes.empty()) {
            std::memcpy(slot->storage.data(), val.bytes.data(), val.bytes.size());
        }
        slot->state = SlotState::Live;
        return slot;
    }

    // #unbound -> most recently created unbound Live direct child slot (or null Pointer)
    PointerLayout top_unbound() const noexcept {
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            const FrameSlot* slot = it->get();
            if (!slot->is_bound && slot->state == SlotState::Live) {
                return slot->pointer();
            }
        }
        return PointerLayout{}; // null
    }

    // #bind(sym, ptr) -> true if ptr is an unbound direct child slot of this frame
    bool bind(SymbolId sym, const PointerLayout& ptr) {
        auto it = addr_to_slot.find(ptr.addr);
        if (it == addr_to_slot.end()) {
            return false;
        }
        FrameSlot* slot = it->second;
        if (slot->is_bound || slot->state == SlotState::Empty) {
            return false;
        }
        // If sym was previously bound, allow rebind only if the old binding is Empty.
        auto existing = sym_to_slot.find(sym);
        if (existing != sym_to_slot.end()) {
            if (existing->second->state != SlotState::Empty) {
                return false;
            }
            existing->second = slot;
        } else {
            sym_to_slot.emplace(sym, slot);
            bind_order.push_back(sym);
        }
        slot->is_bound = true;
        slot->bound_sym = sym;
        return true;
    }

    // #field(#this, sym) -> Pointer to direct child slot (or null if absent / Empty)
    PointerLayout field(SymbolId sym) const noexcept {
        auto it = sym_to_slot.find(sym);
        if (it == sym_to_slot.end()) {
            return PointerLayout{}; // null
        }
        const FrameSlot* slot = it->second;
        if (slot->state == SlotState::Empty) {
            return PointerLayout{}; // null
        }
        return slot->pointer();
    }

    FrameSlot* find_direct_slot(uint64_t addr) const noexcept {
        auto it = addr_to_slot.find(addr);
        return it == addr_to_slot.end() ? nullptr : it->second;
    }

    // #touch(slot_ptr): Uninitialized (or mutable Empty) -> Live without modifying bytes
    void touch(const PointerLayout& ptr) {
        FrameSlot* slot = find_direct_slot(ptr.addr);
        if (!slot) {
            throw std::runtime_error("#touch: target is not a direct child slot of #this");
        }
        if (slot->state == SlotState::Uninitialized ||
            (slot->state == SlotState::Empty && slot->mutability == 1)) {
            slot->state = SlotState::Live;
            return;
        }
        throw std::runtime_error("#touch: invalid slot state transition");
    }

    // #drop(slot_ptr): mark direct child slot in active construction chain (#this or #parent) as Empty (zero RAII)
    void drop_in_chain(const PointerLayout& ptr) {
        ConstructingFrame* cur = this;
        while (cur) {
            FrameSlot* slot = cur->find_direct_slot(ptr.addr);
            if (slot) {
                if (slot->state == SlotState::Empty) {
                    throw std::runtime_error("#drop: double drop on already Empty slot");
                }
                slot->state = SlotState::Empty;
                return;
            }
            cur = cur->parent;
        }
        throw std::runtime_error("#drop: target is not a direct child slot in active construction chain");
    }

    // Block closing phase transition: strip Uninitialized, Empty, and unbound slots;
    // flatten remaining Live bound slots into a single contiguous byte buffer + interned Shape.
    Value close_and_flatten(ShapeArena& shapes) const {
        std::vector<ShapeArena::FieldSpec> specs;
        std::vector<const FrameSlot*> live_slots;

        for (SymbolId sym : bind_order) {
            auto it = sym_to_slot.find(sym);
            if (it == sym_to_slot.end()) continue;
            const FrameSlot* slot = it->second;
            if (slot->state == SlotState::Live) {
                specs.push_back({sym, slot->shape_addr, slot->mutability});
                live_slots.push_back(slot);
            }
        }

        uint64_t closed_shape_addr = shapes.intern_struct_auto(specs);
        const auto* closed_shape = reinterpret_cast<const ShapeLayout*>(closed_shape_addr);

        Value result;
        result.shape_addr = closed_shape_addr;
        result.bytes.resize(closed_shape->size, 0);

        for (size_t i = 0; i < live_slots.size(); ++i) {
            const FieldEntry& fe = closed_shape->fields.entries_begin()[i];
            const FrameSlot* slot = live_slots[i];
            if (!slot->storage.empty()) {
                std::memcpy(result.bytes.data() + fe.offset(), slot->storage.data(), slot->storage.size());
            }
        }
        return result;
    }
};

enum class CoroutineState : uint8_t {
    Suspended,
    Running,
    Dead,
};

struct Coroutine {
    CoroutineState state = CoroutineState::Suspended;
    uint64_t proto_addr = 0;
    Coroutine* caller = nullptr;
    std::vector<std::unique_ptr<ConstructingFrame>> frames;
    std::unique_ptr<FrameSlot> in_slot; // coroutine root mailbox (#in)

    ConstructingFrame* current_frame() noexcept {
        return frames.empty() ? nullptr : frames.back().get();
    }

    ConstructingFrame* push_frame() {
        auto frame = std::make_unique<ConstructingFrame>();
        frame->parent = current_frame();
        ConstructingFrame* raw = frame.get();
        frames.push_back(std::move(frame));
        return raw;
    }

    void pop_frame() {
        if (frames.empty()) {
            throw std::runtime_error("Coroutine::pop_frame: empty frame stack");
        }
        frames.pop_back();
    }
};

} // namespace reverSilly::Lang::Runtime
