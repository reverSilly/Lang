#include "FrameStack.h"

#include <cstring>
#include <stdexcept>

namespace reverSilly::Lang::Runtime {

uint64_t FrameSlot::data_addr() const noexcept {
    if (storage.empty()) {
        return reinterpret_cast<uint64_t>(&storage);
    }
    return reinterpret_cast<uint64_t>(storage.data());
}

PointerLayout FrameSlot::pointer() const noexcept {
    return PointerLayout{data_addr(), shape_addr, mutability};
}

PointerLayout ConstructingFrame::frame_pointer() const noexcept {
    return PointerLayout{frame_addr(), reinterpret_cast<uint64_t>(&live_shape), 1};
}

void ConstructingFrame::sync_live_shape() {
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

FrameSlot* ConstructingFrame::allocate(uint64_t shape_addr, bool is_mut) {
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

FrameSlot* ConstructingFrame::allocate_val(const Value& val, bool is_mut) {
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

bool ConstructingFrame::bind(SymbolId sym, const PointerLayout& ptr) {
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

PointerLayout ConstructingFrame::field(SymbolId sym) const noexcept {
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

FrameSlot* ConstructingFrame::find_direct_slot(uint64_t addr) const noexcept {
    auto it = addr_to_slot.find(addr);
    return it == addr_to_slot.end() ? nullptr : it->second;
}

void ConstructingFrame::touch(const PointerLayout& ptr) {
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

void ConstructingFrame::drop_in_chain(const PointerLayout& ptr) {
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

Value ConstructingFrame::close_and_flatten(ShapeArena& shapes) const {
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

Coroutine::~Coroutine() {
    release_resources();
}

void Coroutine::release_resources() noexcept {
    frames.clear();
    in_slot.reset();
    if (root_handle) {
        root_handle.destroy();
        root_handle = {};
    }
    resume_handle = {};
    state = CoroutineState::Dead;
}

ConstructingFrame* Coroutine::root_frame() noexcept {
    return frames.empty() ? nullptr : frames.front().get();
}

ConstructingFrame* Coroutine::current_frame() noexcept {
    return frames.empty() ? nullptr : frames.back().get();
}

ConstructingFrame* Coroutine::push_frame() {
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

void Coroutine::pop_frame() {
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

void Coroutine::set_in_value(const Value& val) {
    auto slot = std::make_unique<FrameSlot>();
    slot->shape_addr = val.shape_addr;
    slot->mutability = 0;
    slot->state = SlotState::Live;
    slot->storage = val.bytes;
    in_slot = std::move(slot);
}

PointerLayout Coroutine::in_pointer() const noexcept {
    return in_slot ? in_slot->pointer() : PointerLayout{};
}

Coroutine* CoroutinePool::create() {
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

Coroutine* CoroutinePool::resolve(uint64_t handle) const noexcept {
    if (handle == 0) return nullptr;
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (idx >= slots_.size()) return nullptr;
    const Slot& s = slots_[idx];
    if (!s.coro || s.generation != gen) return nullptr;
    return s.coro.get();
}

ConstructingFrame* CoroutinePool::find_open_frame(const PointerLayout& ptr) const noexcept {
    if (ptr.is_null() || ptr.shape_ptr == 0) return nullptr;
    for (const Slot& s : slots_) {
        if (!s.coro) continue;
        for (auto it = s.coro->frames.rbegin(); it != s.coro->frames.rend(); ++it) {
            ConstructingFrame* f = it->get();
            if (f->frame_addr() == ptr.addr &&
                reinterpret_cast<uint64_t>(&f->live_shape) == ptr.shape_ptr) {
                return f;
            }
        }
    }
    return nullptr;
}

bool CoroutinePool::destroy(uint64_t handle) {
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

} // namespace reverSilly::Lang::Runtime
