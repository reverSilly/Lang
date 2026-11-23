#pragma once

#include "Shape.h"
#include "SymbolPool.h"

#include <coroutine>
#include <cstdint>
#include <memory>
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

    uint64_t data_addr() const noexcept;
    PointerLayout pointer() const noexcept;
};

// Active constructing block frame whose live_shape is a standard 40B ShapeLayout.
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

    uint64_t frame_addr() const noexcept { return reinterpret_cast<uint64_t>(this); }
    PointerLayout frame_pointer() const noexcept;

    void sync_live_shape();
    FrameSlot* allocate(uint64_t shape_addr, bool is_mut);
    FrameSlot* allocate_val(const Value& val, bool is_mut);
    bool bind(SymbolId sym, const PointerLayout& ptr);
    PointerLayout field(SymbolId sym) const noexcept;
    FrameSlot* find_direct_slot(uint64_t addr) const noexcept;
    void touch(const PointerLayout& ptr);
    void drop_in_chain(const PointerLayout& ptr);
    Value close_and_flatten(ShapeArena& shapes) const;
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

    ~Coroutine();

    void release_resources() noexcept;
    ConstructingFrame* root_frame() noexcept;
    ConstructingFrame* current_frame() noexcept;
    ConstructingFrame* push_frame();
    void pop_frame();
    void set_in_value(const Value& val);
    PointerLayout in_pointer() const noexcept;
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
    Coroutine* create();
    Coroutine* resolve(uint64_t handle) const noexcept;
    ConstructingFrame* find_open_frame(const PointerLayout& ptr) const noexcept;
    bool destroy(uint64_t handle);
    size_t live_count() const noexcept { return slots_.size() - free_list_.size(); }
};

} // namespace reverSilly::Lang::Runtime
