#include "Evaluator.h"

#include <stdexcept>
#include <vector>

namespace reverSilly::Lang::Eval {

Runtime::Value Evaluator::run(const Ast::ExprPtr& expr) {
    auto entry = run_main_entry(expr);
    rt.active_coro->resume_handle = entry.handle;
    entry.handle.resume();
    rt.flush_pending_reclaim();
    if (entry.handle.promise().exception) {
        std::rethrow_exception(entry.handle.promise().exception);
    }
    return std::move(entry.handle.promise().result);
}

Runtime::Value Evaluator::eval_proto_sync(uint64_t proto_addr) {
    auto entry = run_proto_entry(proto_addr);
    std::coroutine_handle<> prev_handle = rt.active_coro->resume_handle;
    rt.active_coro->resume_handle = entry.handle;
    entry.handle.resume();
    rt.active_coro->resume_handle = prev_handle;
    rt.flush_pending_reclaim();
    if (entry.handle.promise().exception) {
        std::rethrow_exception(entry.handle.promise().exception);
    }
    return std::move(entry.handle.promise().result);
}

uint64_t Evaluator::canonical_shape_addr(const Runtime::ShapeLayout& sl) {
    if (sl.is_leaf()) {
        return rt.shapes.intern_leaf(sl.size, sl.align);
    }
    std::vector<Runtime::FieldEntry> entries;
    entries.reserve(sl.fields.count);
    const Runtime::FieldEntry* arr = sl.fields.entries_begin();
    for (uint32_t i = 0; i < sl.fields.count; ++i) {
        entries.push_back(arr[i]);
    }
    return rt.shapes.intern_struct(entries, sl.size, sl.align);
}

CoroEntry Evaluator::run_main_entry(Ast::ExprPtr expr) {
    try {
        co_return co_await eval_expr(expr);
    } catch (BreakSignal& sig) {
        co_return std::move(sig.value);
    }
}

CoroEntry Evaluator::run_proto_entry(uint64_t proto_addr) {
    try {
        co_return co_await eval_proto(proto_addr);
    } catch (BreakSignal& sig) {
        co_return std::move(sig.value);
    }
}

CoroEntry Evaluator::run_child_coro(Runtime::Coroutine* self, uint64_t proto_addr) {
    Runtime::Value final_val = rt.make_unit();
    try {
        final_val = co_await eval_proto(proto_addr);
    } catch (BreakSignal& sig) {
        final_val = std::move(sig.value);
    } catch (...) {
        self->pop_frame();
        self->state = Runtime::CoroutineState::Dead;
        Runtime::Coroutine* caller = rt.coroutines.resolve(self->caller_handle);
        rt.active_coro = caller;
        rt.pending_reclaim_coro = self->self_handle;
        throw;
    }
    self->pop_frame();
    self->state = Runtime::CoroutineState::Dead;

    Runtime::Coroutine* caller = rt.coroutines.resolve(self->caller_handle);
    rt.pending_reclaim_coro = self->self_handle;
    if (!caller) {
        co_return std::move(final_val);
    }
    caller->set_in_value(final_val);
    caller->state = Runtime::CoroutineState::Running;
    rt.active_coro = caller;

    struct SetNextAwaiter {
        std::coroutine_handle<> target;
        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<CoroEntry::promise_type> h) noexcept {
            h.promise().next_on_finish = target;
            return false;
        }
        void await_resume() noexcept {}
    };
    co_await SetNextAwaiter{caller->resume_handle};
    co_return std::move(final_val);
}

std::coroutine_handle<> Evaluator::ResumeAwaiter::await_suspend(std::coroutine_handle<> current_h) {
    if (!target) {
        throw std::runtime_error("#resume: invalid or dead coroutine handle");
    }
    if (target->state != Runtime::CoroutineState::Suspended) {
        throw std::runtime_error("#resume: target coroutine is not Suspended");
    }
    Runtime::Coroutine* current = ev.rt.active_coro;
    current->resume_handle = current_h;
    current->state = Runtime::CoroutineState::Suspended;

    target->set_in_value(*payload);
    target->caller_handle = current->self_handle;
    target->state = Runtime::CoroutineState::Running;
    ev.rt.active_coro = target;
    return target->resume_handle;
}

Runtime::Value Evaluator::ResumeAwaiter::await_resume() {
    ev.rt.flush_pending_reclaim();
    return ev.rt.make_unit();
}

EvalTask Evaluator::eval_proto(uint64_t proto_addr) {
    const Runtime::PrototypeObject* obj = rt.prototypes.get(proto_addr);
    if (obj->kind == Runtime::CodeKind::NativeSym) {
        if (!obj->native_fn) {
            throw std::runtime_error("#evaluate: unbound native prototype '" + obj->extern_name + "'");
        }
        co_return obj->native_fn(*this);
    }
    co_return co_await eval_expr(obj->ast);
}

EvalTask Evaluator::eval_expr(const Ast::ExprPtr& expr) {
    if (!expr) {
        throw std::runtime_error("Evaluator::eval_expr: null AST expression");
    }
    if (const auto* w = std::get_if<Ast::WordExpr>(&expr->node)) {
        co_return rt.make_symbol(rt.symbols.intern(w->text));
    }
    if (const auto* v = std::get_if<Ast::ValueExpr>(&expr->node)) {
        co_return v->value;
    }
    if (const auto* s = std::get_if<Ast::SequenceExpr>(&expr->node)) {
        if (s->items.empty()) {
            throw std::runtime_error("Evaluator::eval_expr: empty SequenceExpr is forbidden");
        }
        Runtime::Value last = rt.make_unit();
        for (const auto& item : s->items) {
            last = co_await eval_expr(item);
        }
        co_return last;
    }
    if (const auto* b = std::get_if<Ast::BuiltinExpr>(&expr->node)) {
        co_return co_await eval_builtin(*b);
    }
    throw std::runtime_error("Evaluator::eval_expr: unknown AST node");
}

EvalTask Evaluator::eval_builtin(const Ast::BuiltinExpr& b) {
    using K = Ast::BuiltinKind;
    auto expect_shape = [](const Runtime::Value& v, uint64_t expected, const char* msg) {
        if (v.shape_addr != expected) throw std::runtime_error(msg);
    };

    std::vector<Runtime::Value> args;
    args.reserve(b.args.size());
    for (const auto& arg_expr : b.args) {
        args.push_back(co_await eval_expr(arg_expr));
    }

    Runtime::ConstructingFrame* frame = rt.current_frame();

    switch (b.kind) {
        case K::This:
            co_return rt.make_pointer(frame ? frame->frame_pointer() : Runtime::PointerLayout{});
        case K::Parent:
            co_return rt.make_pointer(
                (frame && frame->parent) ? frame->parent->frame_pointer() : Runtime::PointerLayout{});
        case K::Caller:
            co_return rt.make_handle(rt.active_coro->caller_handle);
        case K::In:
            co_return rt.make_pointer(rt.active_coro->in_pointer());

        case K::Evaluate: {
            expect_shape(args[0], rt.shapes.prototype_shape, "#evaluate: argument must be a Prototype");
            co_return co_await eval_proto(args[0].as_pod<uint64_t>());
        }
        case K::Block: {
            expect_shape(args[0], rt.shapes.prototype_shape, "#block: argument must be a Prototype");
            uint64_t proto_addr = args[0].as_pod<uint64_t>();
            rt.active_coro->push_frame();
            try {
                (void)co_await eval_proto(proto_addr);
            } catch (BreakSignal& sig) {
                rt.active_coro->pop_frame();
                co_return std::move(sig.value);
            } catch (...) {
                rt.active_coro->pop_frame();
                throw;
            }
            Runtime::Value flattened = rt.active_coro->current_frame()->close_and_flatten(rt.shapes);
            rt.active_coro->pop_frame();
            co_return flattened;
        }
        case K::New: {
            expect_shape(args[0], rt.shapes.prototype_shape, "#new: argument must be a Prototype");
            uint64_t proto_addr = args[0].as_pod<uint64_t>();
            Runtime::Coroutine* child = rt.coroutines.create();
            child->state = Runtime::CoroutineState::Suspended;
            child->proto_addr = proto_addr;
            child->push_frame();

            CoroEntry entry = run_child_coro(child, proto_addr);
            auto h = entry.release();
            child->root_handle = h;
            child->resume_handle = h;
            co_return rt.make_handle(child->self_handle);
        }
        case K::Destroy: {
            expect_shape(args[0], rt.shapes.handle_shape, "#destroy: argument must be a Handle");
            co_return Runtime::Value::from_pod<uint8_t>(rt.shapes.raw1_shape, rt.coroutines.destroy(args[0].as_pod<uint64_t>()) ? 1 : 0);
        }
        case K::CoroRootFrame: {
            expect_shape(args[0], rt.shapes.handle_shape, "#coro_root_frame: argument must be a Handle");
            Runtime::Coroutine* target = rt.coroutines.resolve(args[0].as_pod<uint64_t>());
            if (!target || !target->root_frame()) {
                co_return rt.make_pointer(Runtime::PointerLayout{});
            }
            co_return rt.make_pointer(target->root_frame()->frame_pointer());
        }

        case K::Allocate: {
            expect_shape(args[0], rt.shapes.shape_meta, "#allocate: arg 0 must be a Shape");
            expect_shape(args[1], rt.shapes.raw1_shape, "#allocate: arg 1 must be bytes(1, 1)");
            auto sl = args[0].as_pod<Runtime::ShapeLayout>();
            uint64_t s_addr = canonical_shape_addr(sl);
            bool is_mut = args[1].as_pod<uint8_t>() != 0;
            Runtime::FrameSlot* slot = frame->allocate(s_addr, is_mut);
            co_return rt.make_pointer(slot->pointer());
        }
        case K::AllocateVal: {
            expect_shape(args[1], rt.shapes.raw1_shape, "#allocate_val: arg 1 must be bytes(1, 1)");
            bool is_mut = args[1].as_pod<uint8_t>() != 0;
            Runtime::FrameSlot* slot = frame->allocate_val(args[0], is_mut);
            co_return rt.make_pointer(slot->pointer());
        }
        case K::Touch: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#touch: argument must be a Pointer");
            frame->touch(args[0].as_pod<Runtime::PointerLayout>());
            co_return rt.make_unit();
        }
        case K::Bind: {
            expect_shape(args[0], rt.shapes.symbol_shape, "#bind: arg 0 must be a Symbol");
            expect_shape(args[1], rt.shapes.pointer_shape, "#bind: arg 1 must be a Pointer");
            bool ok = frame->bind(
                args[0].as_pod<Runtime::SymbolId>(), args[1].as_pod<Runtime::PointerLayout>());
            co_return Runtime::Value::from_pod<uint8_t>(rt.shapes.raw1_shape, ok ? 1 : 0);
        }
        case K::Drop: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#drop: argument must be a Pointer");
            frame->drop_in_chain(args[0].as_pod<Runtime::PointerLayout>());
            co_return rt.make_unit();
        }

        case K::Deref: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#deref: argument must be a Pointer");
            co_return rt.deref(args[0].as_pod<Runtime::PointerLayout>());
        }
        case K::Write: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#write: arg 0 must be a Pointer");
            rt.write(args[0].as_pod<Runtime::PointerLayout>(), args[1]);
            co_return rt.make_unit();
        }

        case K::Field: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#field: arg 0 must be a Pointer");
            expect_shape(args[1], rt.shapes.symbol_shape, "#field: arg 1 must be a Symbol");
            co_return rt.make_pointer(
                rt.field(args[0].as_pod<Runtime::PointerLayout>(), args[1].as_pod<Runtime::SymbolId>()));
        }
        case K::Shape: {
            const Runtime::ShapeLayout* s = args[0].shape();
            if (!s) throw std::runtime_error("#shape: value has null shape");
            co_return Runtime::Value::from_pod(rt.shapes.shape_meta, *s);
        }
        case K::Freeze: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#freeze: argument must be a Pointer");
            Runtime::Value val = rt.deref(args[0].as_pod<Runtime::PointerLayout>());
            Runtime::PointerLayout frozen = rt.shapes.freeze_bytes(val.shape_addr, val.bytes);
            co_return rt.make_pointer(frozen);
        }

        case K::Branch: {
            expect_shape(args[0], rt.shapes.raw1_shape, "#branch: arg 0 must be bytes(1, 1)");
            expect_shape(args[1], rt.shapes.prototype_shape, "#branch: arg 1 must be a Prototype");
            expect_shape(args[2], rt.shapes.prototype_shape, "#branch: arg 2 must be a Prototype");
            bool cond = args[0].as_pod<uint8_t>() != 0;
            uint64_t chosen = (cond ? args[1] : args[2]).as_pod<uint64_t>();
            co_return co_await eval_proto(chosen);
        }
        case K::Break: {
            throw BreakSignal{std::move(args[0])};
        }
        case K::Resume: {
            expect_shape(args[0], rt.shapes.handle_shape, "#resume: arg 0 must be a Handle");
            Runtime::Coroutine* target = rt.coroutines.resolve(args[0].as_pod<uint64_t>());
            co_return co_await ResumeAwaiter{*this, target, &args[1]};
        }
    }
    throw std::runtime_error("Evaluator::eval_builtin: unknown BuiltinKind");
}

} // namespace reverSilly::Lang::Eval
