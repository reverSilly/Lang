#pragma once

#include "Shape.h"
#include "SymbolPool.h"

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Runtime {

struct ConstructingFrame;

struct FrameSlot {
    ConstructingFrame* owner = nullptr;
    uint64_t shape_addr = 0;
    uint8_t mutability = 0;
    SlotState state = SlotState::Uninitialized;
    bool is_bound = false;
    SymbolId bound_sym = 0;
    std::vector<uint8_t> storage;

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

// Active constructing block frame whose live_shape is a standard 40B ShapeLayout:
// - #this / #parent / #coro_root_frame return regular fat pointers { frame_addr, &live_shape, 1 }.
// - live_entries_ keeps all slots (bound, unbound, 3-state) PLUS at most one active child open frame.
// - Each entry stores relative offset (slot_addr - frame_addr) mod 2^64 so #field uses ONE unified path.
struct ConstructingFrame {
    ConstructingFrame* parent = nullptr;
    ConstructingFrame* active_child = nullptr;
    std::vector<std::unique_ptr<FrameSlot>> slots;
    std::unordered_map<uint64_t, FrameSlot*> addr_to_slot;
    std::unordered_map<SymbolId, FrameSlot*> sym_to_slot;
    std::vector<SymbolId> bind_order;

    // Live unified ShapeLayout describing this open frame (size=0 indicates open frame)
    ShapeLayout live_shape{};
    std::vector<FieldEntry> live_entries_;
    std::vector<uint32_t> live_buckets_;

    uint64_t frame_addr() const noexcept {
        return reinterpret_cast<uint64_t>(this);
    }

    PointerLayout frame_pointer() const noexcept {
        return PointerLayout{frame_addr(), reinterpret_cast<uint64_t>(&live_shape), 1};
    }

    void sync_live_shape() {
        live_entries_.clear();
        live_entries_.reserve(slots.size() + (active_child ? 1 : 0));
        uint64_t base = frame_addr();
        for (const auto& s_ptr : slots) {
            const FrameSlot* s = s_ptr.get();
            uint64_t delta = s->data_addr() - base; // wrapping mod 2^64
            live_entries_.push_back(FieldEntry{
                s->is_bound ? s->bound_sym : 0u,
                static_cast<uint8_t>(s->state),
                {0, 0, 0},
                PointerLayout{delta, s->shape_addr, s->mutability},
            });
        }
        if (active_child) {
            uint64_t child_delta = active_child->frame_addr() - base;
            live_entries_.push_back(FieldEntry{
                0u, // anonymous active child frame slot
                static_cast<uint8_t>(SlotState::Live),
                {0, 0, 0},
                PointerLayout{child_delta, reinterpret_cast<uint64_t>(&active_child->live_shape), 1},
            });
        }
        live_shape.size = 0;
        live_shape.align = 1;
        live_shape.fields = FieldMapInline::build_view(live_entries_, live_buckets_);
    }

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
        sync_live_shape();
        return raw;
    }

    FrameSlot* allocate_val(const Value& val, bool is_mut) {
        FrameSlot* slot = allocate(val.shape_addr, is_mut);
        if (val.bytes.size() != slot->storage.size()) {
            throw std::runtime_error("ConstructingFrame::allocate_val: size mismatch");
        }
        if (!val.bytes.empty()) {
            std::memcpy(slot->storage.data(), val.bytes.data(), val.bytes.size());
        }
        slot->state = SlotState::Live;
        sync_live_shape();
        return slot;
    }

    bool bind(SymbolId sym, const PointerLayout& ptr) {
        auto it = addr_to_slot.find(ptr.addr);
        if (it == addr_to_slot.end()) {
            return false;
        }
        FrameSlot* slot = it->second;
        if (slot->is_bound || slot->state == SlotState::Empty) {
            return false;
        }
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
        sync_live_shape();
        return true;
    }

    PointerLayout field(SymbolId sym) const noexcept {
        auto it = sym_to_slot.find(sym);
        if (it == sym_to_slot.end()) {
            return PointerLayout{};
        }
        const FrameSlot* slot = it->second;
        if (slot->state == SlotState::Empty) {
            return PointerLayout{};
        }
        return slot->pointer();
    }

    FrameSlot* find_direct_slot(uint64_t addr) const noexcept {
        auto it = addr_to_slot.find(addr);
        return it == addr_to_slot.end() ? nullptr : it->second;
    }

    void touch(const PointerLayout& ptr) {
        FrameSlot* slot = find_direct_slot(ptr.addr);
        if (!slot) {
            throw std::runtime_error("#touch: target is not a direct child slot of #this");
        }
        if (slot->state == SlotState::Uninitialized ||
            (slot->state == SlotState::Empty && slot->mutability == 1)) {
            slot->state = SlotState::Live;
            sync_live_shape();
            return;
        }
        throw std::runtime_error("#touch: invalid slot state transition");
    }

    void drop_in_chain(const PointerLayout& ptr) {
        ConstructingFrame* cur = this;
        while (cur) {
            FrameSlot* slot = cur->find_direct_slot(ptr.addr);
            if (slot) {
                if (slot->state == SlotState::Empty) {
                    throw std::runtime_error("#drop: double drop on already Empty slot");
                }
                slot->state = SlotState::Empty;
                cur->sync_live_shape();
                return;
            }
            cur = cur->parent;
        }
        throw std::runtime_error("#drop: target is not a direct child slot in active construction chain");
    }

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
    uint64_t self_handle = 0;
    uint64_t caller_handle = 0;
    CoroutineState state = CoroutineState::Suspended;
    uint64_t proto_addr = 0;
    std::coroutine_handle<> resume_handle{};
    std::coroutine_handle<> root_handle{};
    std::vector<std::unique_ptr<ConstructingFrame>> frames;
    std::unique_ptr<FrameSlot> in_slot;

    ~Coroutine() {
        release_resources();
    }

    void release_resources() noexcept {
        frames.clear();
        in_slot.reset();
        if (root_handle) {
            root_handle.destroy();
            root_handle = {};
        }
        resume_handle = {};
        state = CoroutineState::Dead;
    }

    ConstructingFrame* root_frame() noexcept {
        return frames.empty() ? nullptr : frames.front().get();
    }

    ConstructingFrame* current_frame() noexcept {
        return frames.empty() ? nullptr : frames.back().get();
    }

    ConstructingFrame* push_frame() {
        ConstructingFrame* par = current_frame();
        auto frame = std::make_unique<ConstructingFrame>();
        frame->parent = par;
        ConstructingFrame* raw = frame.get();
        frames.push_back(std::move(frame));
        if (par) {
            par->active_child = raw;
            par->sync_live_shape();
        }
        return raw;
    }

    void pop_frame() {
        if (frames.empty()) {
            throw std::runtime_error("Coroutine::pop_frame: empty frame stack");
        }
        ConstructingFrame* par = frames.back()->parent;
        frames.pop_back();
        if (par) {
            par->active_child = nullptr;
            par->sync_live_shape();
        }
    }

    void set_in_value(const Value& val) {
        auto slot = std::make_unique<FrameSlot>();
        slot->shape_addr = val.shape_addr;
        slot->mutability = 0;
        slot->state = SlotState::Live;
        slot->storage = val.bytes;
        in_slot = std::move(slot);
    }

    PointerLayout in_pointer() const noexcept {
        return in_slot ? in_slot->pointer() : PointerLayout{};
    }
};

class CoroutinePool {
private:
    struct Slot {
        uint32_t generation = 1;
        std::unique_ptr<Coroutine> coro;
    };

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_list_;

    static uint64_t encode(uint32_t idx, uint32_t gen) noexcept {
        return (static_cast<uint64_t>(gen) << 32) | static_cast<uint64_t>(idx);
    }

public:
    Coroutine* create() {
        uint32_t idx = 0;
        if (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
        } else {
            idx = static_cast<uint32_t>(slots_.size());
            slots_.push_back(Slot{1, nullptr});
        }
        auto coro = std::make_unique<Coroutine>();
        coro->self_handle = encode(idx, slots_[idx].generation);
        Coroutine* raw = coro.get();
        slots_[idx].coro = std::move(coro);
        return raw;
    }

    Coroutine* resolve(uint64_t handle) const noexcept {
        if (handle == 0) return nullptr;
        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
        uint32_t gen = static_cast<uint32_t>(handle >> 32);
        if (idx >= slots_.size()) return nullptr;
        const Slot& s = slots_[idx];
        if (!s.coro || s.generation != gen) return nullptr;
        return s.coro.get();
    }

    bool destroy(uint64_t handle) {
        if (handle == 0) return false;
        uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
        uint32_t gen = static_cast<uint32_t>(handle >> 32);
        if (idx >= slots_.size()) return false;
        Slot& s = slots_[idx];
        if (!s.coro || s.generation != gen) return false;
        if (s.coro->state == CoroutineState::Running) {
            throw std::runtime_error("CoroutinePool::destroy: cannot destroy a Running coroutine");
        }
        s.coro.reset();
        ++s.generation;
        if (s.generation == 0) s.generation = 1;
        free_list_.push_back(idx);
        return true;
    }

    size_t live_count() const noexcept {
        return slots_.size() - free_list_.size();
    }
};

} // namespace reverSilly::Lang::Runtime
