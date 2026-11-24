#include "Evaluator.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace reverSilly::Lang::Eval {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

uint64_t raw_shape_for_width(const Runtime::ShapeArena& shapes, uint8_t width) {
    switch (width) {
        case 1: return shapes.raw1_shape;
        case 2: return shapes.raw2_shape;
        case 4: return shapes.raw4_shape;
        case 8: return shapes.raw8_shape;
        default:
            throw std::runtime_error("intrinsic: unsupported byte width " + std::to_string(width));
    }
}

Runtime::Value read_slot_bytes(Runtime::RuntimeContext& rt, Runtime::SymbolId sym, uint8_t expected_width) {
    Runtime::ConstructingFrame* f = rt.current_frame();
    if (!f) throw std::runtime_error("intrinsic: no active frame");
    Runtime::PointerLayout p = f->field(sym);
    if (p.is_null()) {
        throw std::runtime_error("intrinsic: missing slot '" + std::string(rt.symbols.name(sym)) + "' in #this");
    }
    Runtime::Value v = rt.deref(p);
    if (v.bytes.size() != expected_width) {
        throw std::runtime_error(
            "intrinsic: slot '" + std::string(rt.symbols.name(sym)) + "' byte width mismatch (got " +
            std::to_string(v.bytes.size()) + ", expected " + std::to_string(expected_width) + ")");
    }
    return v;
}

template <typename Fn>
decltype(auto) dispatch_sint(uint8_t width, Fn&& fn) {
    switch (width) {
        case 1: return fn.template operator()<int8_t>();
        case 2: return fn.template operator()<int16_t>();
        case 4: return fn.template operator()<int32_t>();
        case 8: return fn.template operator()<int64_t>();
    }
    throw std::runtime_error("intrinsic: unsupported signed integer width " + std::to_string(width));
}

template <typename Fn>
decltype(auto) dispatch_uint(uint8_t width, Fn&& fn) {
    switch (width) {
        case 1: return fn.template operator()<uint8_t>();
        case 2: return fn.template operator()<uint16_t>();
        case 4: return fn.template operator()<uint32_t>();
        case 8: return fn.template operator()<uint64_t>();
    }
    throw std::runtime_error("intrinsic: unsupported unsigned integer width " + std::to_string(width));
}

template <typename Fn>
decltype(auto) dispatch_float(uint8_t width, Fn&& fn) {
    switch (width) {
        case 4: return fn.template operator()<float>();
        case 8: return fn.template operator()<double>();
    }
    throw std::runtime_error("intrinsic: unsupported float width " + std::to_string(width));
}


Ast::AstShape reify_shape(const Runtime::RuntimeContext& rt, const Runtime::ShapeLayout* sl) {
    if (!sl) {
        throw std::runtime_error("reify_shape: null ShapeLayout");
    }
    Ast::AstShape out;
    out.size = sl->size;
    out.align = sl->align;
    if (!sl->is_leaf()) {
        const Runtime::FieldEntry* arr = sl->fields.entries_begin();
        out.fields.reserve(sl->fields.count);
        for (uint32_t i = 0; i < sl->fields.count; ++i) {
            const auto* sub = reinterpret_cast<const Runtime::ShapeLayout*>(arr[i].sub_shape_addr());
            out.fields.push_back(Ast::AstField{
                std::string(rt.symbols.name(arr[i].sym)),
                arr[i].offset(),
                reify_shape(rt, sub),
            });
        }
    }
    return out;
}

uint64_t intern_ast_shape(Runtime::RuntimeContext& rt, const Ast::AstShape& s) {
    if (s.fields.empty()) {
        return rt.shapes.intern_leaf(s.size, s.align);
    }
    std::vector<Runtime::FieldEntry> entries;
    entries.reserve(s.fields.size());
    for (const auto& f : s.fields) {
        Runtime::FieldEntry fe{};
        fe.sym = rt.symbols.intern(f.name);
        fe.state = static_cast<uint8_t>(Runtime::SlotState::Live);
        fe.rel_ptr = Runtime::PointerLayout{f.offset, intern_ast_shape(rt, f.shape), 0};
        entries.push_back(fe);
    }
    return rt.shapes.intern_struct(entries, s.size, s.align);
}

Ast::AstIntrinsicSpec reify_intrinsic(
    const Runtime::RuntimeContext& rt, const Runtime::IntrinsicSpec& spec) {
    return std::visit(Overloaded{
        [&](const Runtime::IntBinOp& op) -> Ast::AstIntrinsicSpec {
            return Ast::AstIntBinOp{
                op.op, op.width, std::string(rt.symbols.name(op.lhs)), std::string(rt.symbols.name(op.rhs))};
        },
        [&](const Runtime::IntUnaryOp& op) -> Ast::AstIntrinsicSpec {
            return Ast::AstIntUnaryOp{op.op, op.width, std::string(rt.symbols.name(op.src))};
        },
        [&](const Runtime::FloatBinOp& op) -> Ast::AstIntrinsicSpec {
            return Ast::AstFloatBinOp{
                op.op, op.width, std::string(rt.symbols.name(op.lhs)), std::string(rt.symbols.name(op.rhs))};
        },
        [&](const Runtime::CastOp& op) -> Ast::AstIntrinsicSpec {
            return Ast::AstCastOp{op.op, op.from_width, op.to_width, std::string(rt.symbols.name(op.src))};
        },
        [&](const Runtime::CmpOp& op) -> Ast::AstIntrinsicSpec {
            return Ast::AstCmpOp{
                op.op, op.width, std::string(rt.symbols.name(op.lhs)), std::string(rt.symbols.name(op.rhs))};
        },
    }, spec);
}

Runtime::IntrinsicSpec intern_intrinsic(
    Runtime::RuntimeContext& rt, const Ast::AstIntrinsicSpec& spec) {
    return std::visit(Overloaded{
        [&](const Ast::AstIntBinOp& op) -> Runtime::IntrinsicSpec {
            return Runtime::IntBinOp{op.op, op.width, rt.symbols.intern(op.lhs), rt.symbols.intern(op.rhs)};
        },
        [&](const Ast::AstIntUnaryOp& op) -> Runtime::IntrinsicSpec {
            return Runtime::IntUnaryOp{op.op, op.width, rt.symbols.intern(op.src)};
        },
        [&](const Ast::AstFloatBinOp& op) -> Runtime::IntrinsicSpec {
            return Runtime::FloatBinOp{op.op, op.width, rt.symbols.intern(op.lhs), rt.symbols.intern(op.rhs)};
        },
        [&](const Ast::AstCastOp& op) -> Runtime::IntrinsicSpec {
            return Runtime::CastOp{op.op, op.from_width, op.to_width, rt.symbols.intern(op.src)};
        },
        [&](const Ast::AstCmpOp& op) -> Runtime::IntrinsicSpec {
            return Runtime::CmpOp{op.op, op.width, rt.symbols.intern(op.lhs), rt.symbols.intern(op.rhs)};
        },
    }, spec);
}

Runtime::ShapeLayout read_slot_shape(Runtime::RuntimeContext& rt, Runtime::SymbolId sym) {
    Runtime::ConstructingFrame* f = rt.current_frame();
    if (!f) throw std::runtime_error("intrinsic: no active frame");
    Runtime::PointerLayout p = f->field(sym);
    if (p.is_null()) {
        throw std::runtime_error("intrinsic: missing slot '" + std::string(rt.symbols.name(sym)) + "' in #this");
    }
    Runtime::Value v = rt.deref(p);
    if (v.shape_addr != rt.shapes.shape_meta) {
        throw std::runtime_error(
            "intrinsic eq_shape: slot '" + std::string(rt.symbols.name(sym)) + "' must be a Shape");
    }
    return v.as_pod<Runtime::ShapeLayout>();
}

} // namespace

Ast::AstValue reify_ast_value(const Runtime::RuntimeContext& rt, const Runtime::Value& val) {
    if (val.shape_addr == rt.shapes.symbol_shape) {
        auto id = val.as_pod<Runtime::SymbolId>();
        return Ast::AstValue{Ast::SymbolVal{std::string(rt.symbols.name(id))}};
    }
    if (val.shape_addr == rt.shapes.shape_meta) {
        auto sl = val.as_pod<Runtime::ShapeLayout>();
        return Ast::AstValue{Ast::ShapeVal{reify_shape(rt, &sl)}};
    }
    if (val.shape_addr == rt.shapes.pointer_shape) {
        throw std::runtime_error(
            "ast.value: Pointer is runtime-bound and cannot be embedded in standalone Ast");
    }
    if (val.shape_addr == rt.shapes.handle_shape) {
        throw std::runtime_error(
            "ast.value: Handle is runtime-bound and cannot be embedded in standalone Ast");
    }
    if (val.shape_addr == rt.shapes.prototype_shape) {
        const Runtime::PrototypeObject* obj = rt.prototypes.get(val.as_pod<uint64_t>());
        return std::visit(Overloaded{
            [&](const Runtime::AstProto& p) -> Ast::AstValue {
                return Ast::AstValue{Ast::AstProtoVal{p.ast}};
            },
            [&](const Runtime::IntrinsicProto& p) -> Ast::AstValue {
                return Ast::AstValue{Ast::IntrinsicVal{reify_intrinsic(rt, p.spec)}};
            },
            [&](const Runtime::FfiProto& p) -> Ast::AstValue {
                const auto* args_s = reinterpret_cast<const Runtime::ShapeLayout*>(p.binding.args_shape_addr);
                const auto* ret_s = reinterpret_cast<const Runtime::ShapeLayout*>(p.binding.ret_shape_addr);
                return Ast::AstValue{Ast::FfiExternVal{
                    p.binding.lib_name,
                    p.binding.symbol_name,
                    reify_shape(rt, args_s),
                    reify_shape(rt, ret_s),
                    std::string(rt.symbols.name(p.binding.cc)),
                }};
            },
            [&](const Runtime::FfiBindPtrProto& p) -> Ast::AstValue {
                const auto* args_s = reinterpret_cast<const Runtime::ShapeLayout*>(p.spec.args_shape_addr);
                const auto* ret_s = reinterpret_cast<const Runtime::ShapeLayout*>(p.spec.ret_shape_addr);
                return Ast::AstValue{Ast::FfiBindPtrVal{
                    std::string(rt.symbols.name(p.spec.fn_slot)),
                    reify_shape(rt, args_s),
                    reify_shape(rt, ret_s),
                    std::string(rt.symbols.name(p.spec.cc)),
                }};
            },
            [&](const Runtime::NativeProto& p) -> Ast::AstValue {
                throw std::runtime_error(
                    "ast.value: NativeProto '" + p.extern_name +
                    "' is host-only and cannot be embedded in standalone Ast");
            },
            [&](const Runtime::BoundFfiProto&) -> Ast::AstValue {
                throw std::runtime_error(
                    "ast.value: BoundFfiProto holds a runtime function pointer and cannot be embedded in standalone Ast");
            },
        }, obj->payload);
    }

    const Runtime::ShapeLayout* sl = val.shape();
    if (!sl) {
        throw std::runtime_error("ast.value: value has null Shape");
    }
    if (sl->is_leaf()) {
        return Ast::AstValue{Ast::RawBytes{sl->align, val.bytes}};
    }

    Ast::StructVal st;
    st.size = sl->size;
    st.align = sl->align;
    const Runtime::FieldEntry* arr = sl->fields.entries_begin();
    st.fields.reserve(sl->fields.count);
    for (uint32_t i = 0; i < sl->fields.count; ++i) {
        const auto* sub = reinterpret_cast<const Runtime::ShapeLayout*>(arr[i].sub_shape_addr());
        Runtime::Value fv;
        fv.shape_addr = arr[i].sub_shape_addr();
        if (sub && sub->size > 0) {
            fv.bytes.assign(
                val.bytes.begin() + static_cast<std::ptrdiff_t>(arr[i].offset()),
                val.bytes.begin() + static_cast<std::ptrdiff_t>(arr[i].offset() + sub->size));
        }
        st.fields.push_back(Ast::AstStructField{
            std::string(rt.symbols.name(arr[i].sym)),
            arr[i].offset(),
            reify_ast_value(rt, fv),
        });
    }
    return Ast::AstValue{std::move(st)};
}

Runtime::Value intern_ast_value(Runtime::RuntimeContext& rt, const Ast::AstValue& av) {
    return std::visit(Overloaded{
        [&](const Ast::RawBytes& rb) -> Runtime::Value {
            uint64_t sh = rt.shapes.intern_leaf(rb.bytes.size(), rb.align);
            return Runtime::Value{sh, rb.bytes};
        },
        [&](const Ast::SymbolVal& sv) -> Runtime::Value {
            return rt.make_symbol(rt.symbols.intern(sv.name));
        },
        [&](const Ast::ShapeVal& sv) -> Runtime::Value {
            uint64_t addr = intern_ast_shape(rt, sv.shape);
            const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(addr);
            return Runtime::Value::from_pod(rt.shapes.shape_meta, *sl);
        },
        [&](const Ast::AstProtoVal& pv) -> Runtime::Value {
            return rt.make_proto(rt.prototypes.create_ast(pv.ast));
        },
        [&](const Ast::IntrinsicVal& iv) -> Runtime::Value {
            return rt.make_proto(rt.prototypes.create_intrinsic(intern_intrinsic(rt, iv.spec)));
        },
        [&](const Ast::FfiExternVal& fv) -> Runtime::Value {
            Runtime::FfiBinding b{
                fv.lib_name,
                fv.symbol_name,
                intern_ast_shape(rt, fv.args),
                intern_ast_shape(rt, fv.ret),
                rt.symbols.intern(fv.cc),
            };
            return rt.make_proto(rt.prototypes.create_ffi_extern(std::move(b)));
        },
        [&](const Ast::FfiBindPtrVal& bv) -> Runtime::Value {
            Runtime::FfiBindPtrSpec spec{
                rt.symbols.intern(bv.fn_slot),
                intern_ast_shape(rt, bv.args),
                intern_ast_shape(rt, bv.ret),
                rt.symbols.intern(bv.cc),
            };
            return rt.make_proto(rt.prototypes.create_ffi_bind_ptr(spec));
        },
        [&](const Ast::StructVal& st) -> Runtime::Value {
            std::vector<uint8_t> buf(st.size, 0);
            std::vector<Runtime::FieldEntry> entries;
            entries.reserve(st.fields.size());
            for (const auto& f : st.fields) {
                Runtime::Value fv = intern_ast_value(rt, f.value);
                if (!fv.bytes.empty()) {
                    std::memcpy(buf.data() + f.offset, fv.bytes.data(), fv.bytes.size());
                }
                Runtime::FieldEntry fe{};
                fe.sym = rt.symbols.intern(f.name);
                fe.state = static_cast<uint8_t>(Runtime::SlotState::Live);
                fe.rel_ptr = Runtime::PointerLayout{f.offset, fv.shape_addr, 0};
                entries.push_back(fe);
            }
            uint64_t sh = rt.shapes.intern_struct(entries, st.size, st.align);
            return Runtime::Value{sh, std::move(buf)};
        },
    }, av.data);
}

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

Runtime::Value Evaluator::eval_intrinsic(const Runtime::IntrinsicSpec& spec) {
    auto raw_out = [&](uint8_t w, const auto& val) {
        return Runtime::Value::from_pod(raw_shape_for_width(rt.shapes, w), val);
    };
    auto flag_out = [&](bool b) {
        return Runtime::Value::from_pod<uint8_t>(rt.shapes.raw1_shape, b ? 1 : 0);
    };

    return std::visit(Overloaded{
        [&](const Runtime::IntBinOp& op) -> Runtime::Value {
            using K = Runtime::IntBinKind;
            auto a_val = read_slot_bytes(rt, op.lhs, op.width);
            auto b_val = read_slot_bytes(rt, op.rhs, op.width);
            switch (op.op) {
                case K::Add:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() + b_val.as_pod<U>()));
                    });
                case K::Sub:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() - b_val.as_pod<U>()));
                    });
                case K::Mul:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() * b_val.as_pod<U>()));
                    });
                case K::SDiv:
                    return dispatch_sint(op.width, [&]<typename S>() {
                        S a = a_val.as_pod<S>(), b = b_val.as_pod<S>();
                        if (b == 0) throw std::runtime_error("intrinsic sdiv: division by zero");
                        if (a == std::numeric_limits<S>::min() && b == static_cast<S>(-1)) {
                            throw std::runtime_error("intrinsic sdiv: signed overflow");
                        }
                        return raw_out(op.width, static_cast<S>(a / b));
                    });
                case K::UDiv:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        U a = a_val.as_pod<U>(), b = b_val.as_pod<U>();
                        if (b == 0) throw std::runtime_error("intrinsic udiv: division by zero");
                        return raw_out(op.width, static_cast<U>(a / b));
                    });
                case K::SRem:
                    return dispatch_sint(op.width, [&]<typename S>() {
                        S a = a_val.as_pod<S>(), b = b_val.as_pod<S>();
                        if (b == 0) throw std::runtime_error("intrinsic srem: division by zero");
                        if (a == std::numeric_limits<S>::min() && b == static_cast<S>(-1)) {
                            return raw_out(op.width, static_cast<S>(0));
                        }
                        return raw_out(op.width, static_cast<S>(a % b));
                    });
                case K::URem:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        U a = a_val.as_pod<U>(), b = b_val.as_pod<U>();
                        if (b == 0) throw std::runtime_error("intrinsic urem: division by zero");
                        return raw_out(op.width, static_cast<U>(a % b));
                    });
                case K::BitAnd:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() & b_val.as_pod<U>()));
                    });
                case K::BitOr:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() | b_val.as_pod<U>()));
                    });
                case K::BitXor:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(a_val.as_pod<U>() ^ b_val.as_pod<U>()));
                    });
                case K::Shl:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        U a = a_val.as_pod<U>(), b = b_val.as_pod<U>();
                        if (b >= sizeof(U) * 8) throw std::runtime_error("intrinsic shl: shift amount out of range");
                        return raw_out(op.width, static_cast<U>(a << b));
                    });
                case K::AShr:
                    return dispatch_sint(op.width, [&]<typename S>() {
                        using U = std::make_unsigned_t<S>;
                        S a = a_val.as_pod<S>();
                        U b = b_val.as_pod<U>();
                        if (b >= sizeof(S) * 8) throw std::runtime_error("intrinsic ashr: shift amount out of range");
                        return raw_out(op.width, static_cast<S>(a >> b));
                    });
                case K::LShr:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        U a = a_val.as_pod<U>(), b = b_val.as_pod<U>();
                        if (b >= sizeof(U) * 8) throw std::runtime_error("intrinsic lshr: shift amount out of range");
                        return raw_out(op.width, static_cast<U>(a >> b));
                    });
            }
            throw std::runtime_error("Evaluator::eval_intrinsic: unknown IntBinKind");
        },

        [&](const Runtime::IntUnaryOp& op) -> Runtime::Value {
            auto src_val = read_slot_bytes(rt, op.src, op.width);
            switch (op.op) {
                case Runtime::IntUnaryKind::BitNot:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return raw_out(op.width, static_cast<U>(~src_val.as_pod<U>()));
                    });
            }
            throw std::runtime_error("Evaluator::eval_intrinsic: unknown IntUnaryKind");
        },

        [&](const Runtime::FloatBinOp& op) -> Runtime::Value {
            using K = Runtime::FloatBinKind;
            auto a_val = read_slot_bytes(rt, op.lhs, op.width);
            auto b_val = read_slot_bytes(rt, op.rhs, op.width);
            return dispatch_float(op.width, [&]<typename F>() {
                F a = a_val.as_pod<F>(), b = b_val.as_pod<F>();
                switch (op.op) {
                    case K::Add: return raw_out(op.width, a + b);
                    case K::Sub: return raw_out(op.width, a - b);
                    case K::Mul: return raw_out(op.width, a * b);
                    case K::Div: return raw_out(op.width, a / b);
                }
                throw std::runtime_error("Evaluator::eval_intrinsic: unknown FloatBinKind");
            });
        },

        [&](const Runtime::CastOp& op) -> Runtime::Value {
            using K = Runtime::CastKind;
            auto src_val = read_slot_bytes(rt, op.src, op.from_width);
            switch (op.op) {
                case K::SExt:
                    return dispatch_sint(op.from_width, [&]<typename Src>() {
                        Src v = src_val.as_pod<Src>();
                        return dispatch_sint(op.to_width, [&]<typename Dst>() {
                            return raw_out(op.to_width, static_cast<Dst>(v));
                        });
                    });
                case K::ZExt:
                    return dispatch_uint(op.from_width, [&]<typename Src>() {
                        Src v = src_val.as_pod<Src>();
                        return dispatch_uint(op.to_width, [&]<typename Dst>() {
                            return raw_out(op.to_width, static_cast<Dst>(v));
                        });
                    });
                case K::Trunc:
                    return dispatch_uint(op.from_width, [&]<typename Src>() {
                        Src v = src_val.as_pod<Src>();
                        return dispatch_uint(op.to_width, [&]<typename Dst>() {
                            return raw_out(op.to_width, static_cast<Dst>(v));
                        });
                    });
                case K::FPExt:
                    return raw_out(8, static_cast<double>(src_val.as_pod<float>()));
                case K::FPTrunc:
                    return raw_out(4, static_cast<float>(src_val.as_pod<double>()));
                case K::SIToFP:
                    return dispatch_sint(op.from_width, [&]<typename I>() {
                        I v = src_val.as_pod<I>();
                        return dispatch_float(op.to_width, [&]<typename F>() {
                            return raw_out(op.to_width, static_cast<F>(v));
                        });
                    });
                case K::UIToFP:
                    return dispatch_uint(op.from_width, [&]<typename I>() {
                        I v = src_val.as_pod<I>();
                        return dispatch_float(op.to_width, [&]<typename F>() {
                            return raw_out(op.to_width, static_cast<F>(v));
                        });
                    });
                case K::FPToSI:
                    return dispatch_float(op.from_width, [&]<typename F>() {
                        F v = src_val.as_pod<F>();
                        return dispatch_sint(op.to_width, [&]<typename I>() {
                            return raw_out(op.to_width, static_cast<I>(v));
                        });
                    });
                case K::FPToUI:
                    return dispatch_float(op.from_width, [&]<typename F>() {
                        F v = src_val.as_pod<F>();
                        return dispatch_uint(op.to_width, [&]<typename I>() {
                            return raw_out(op.to_width, static_cast<I>(v));
                        });
                    });
            }
            throw std::runtime_error("Evaluator::eval_intrinsic: unknown CastKind");
        },

        [&](const Runtime::CmpOp& op) -> Runtime::Value {
            using K = Runtime::CmpKind;
            if (op.op == K::EqShape) {
                auto a_sl = read_slot_shape(rt, op.lhs);
                auto b_sl = read_slot_shape(rt, op.rhs);
                return flag_out(canonical_shape_addr(a_sl) == canonical_shape_addr(b_sl));
            }
            auto a_val = read_slot_bytes(rt, op.lhs, op.width);
            auto b_val = read_slot_bytes(rt, op.rhs, op.width);
            switch (op.op) {
                case K::EqShape:
                    break;
                case K::EqBytes:
                    return flag_out(std::memcmp(a_val.bytes.data(), b_val.bytes.data(), op.width) == 0);
                case K::SLt:
                    return dispatch_sint(op.width, [&]<typename S>() {
                        return flag_out(a_val.as_pod<S>() < b_val.as_pod<S>());
                    });
                case K::ULt:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return flag_out(a_val.as_pod<U>() < b_val.as_pod<U>());
                    });
                case K::SLe:
                    return dispatch_sint(op.width, [&]<typename S>() {
                        return flag_out(a_val.as_pod<S>() <= b_val.as_pod<S>());
                    });
                case K::ULe:
                    return dispatch_uint(op.width, [&]<typename U>() {
                        return flag_out(a_val.as_pod<U>() <= b_val.as_pod<U>());
                    });
                case K::FEq:
                    return dispatch_float(op.width, [&]<typename F>() {
                        return flag_out(a_val.as_pod<F>() == b_val.as_pod<F>());
                    });
                case K::FLt:
                    return dispatch_float(op.width, [&]<typename F>() {
                        return flag_out(a_val.as_pod<F>() < b_val.as_pod<F>());
                    });
                case K::FLe:
                    return dispatch_float(op.width, [&]<typename F>() {
                        return flag_out(a_val.as_pod<F>() <= b_val.as_pod<F>());
                    });
            }
            throw std::runtime_error("Evaluator::eval_intrinsic: unknown CmpKind");
        },
    }, spec);
}

EvalTask Evaluator::eval_proto(uint64_t proto_addr) {
    const Runtime::PrototypeObject* obj = rt.prototypes.get(proto_addr);
    if (const auto* ast_p = std::get_if<Runtime::AstProto>(&obj->payload)) {
        co_return co_await eval_expr(ast_p->ast);
    }
    // Exhaustive std::visit across all PrototypeObject variants (compile-time checked).
    co_return std::visit(Overloaded{
        [](const Runtime::AstProto&) -> Runtime::Value {
            throw std::logic_error("Evaluator::eval_proto: AstProto handled above");
        },
        [&](const Runtime::NativeProto& p) -> Runtime::Value {
            if (!p.fn) {
                throw std::runtime_error("#evaluate: unbound native prototype '" + p.extern_name + "'");
            }
            return p.fn(*this);
        },
        [&](const Runtime::FfiProto& p) -> Runtime::Value {
            return rt.ffi_cache.call_extern(rt, p.binding);
        },
        [&](const Runtime::FfiBindPtrProto& p) -> Runtime::Value {
            Runtime::Value fn_val = read_slot_bytes(rt, p.spec.fn_slot, 8);
            void* fn_ptr = reinterpret_cast<void*>(fn_val.as_pod<uint64_t>());
            if (!fn_ptr) {
                throw std::runtime_error("#evaluate: ffi.bind_ptr received null function pointer");
            }
            return rt.make_proto(rt.prototypes.create_bound_ffi(Runtime::BoundFfiSpec{
                fn_ptr, p.spec.args_shape_addr, p.spec.ret_shape_addr, p.spec.cc}));
        },
        [&](const Runtime::BoundFfiProto& p) -> Runtime::Value {
            return rt.ffi_cache.call_bound(rt, p.spec);
        },
        [&](const Runtime::IntrinsicProto& p) -> Runtime::Value {
            return eval_intrinsic(p.spec);
        },
    }, obj->payload);
}

EvalTask Evaluator::eval_expr(const Ast::ExprPtr& expr) {
    if (!expr) {
        throw std::runtime_error("Evaluator::eval_expr: null AST expression");
    }
    if (const auto* w = std::get_if<Ast::WordExpr>(&expr->node)) {
        co_return rt.make_symbol(rt.symbols.intern(w->text));
    }
    if (const auto* v = std::get_if<Ast::ValueExpr>(&expr->node)) {
        co_return intern_ast_value(rt, v->value);
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
    auto describe_shape = [&](uint64_t shape_addr) -> std::string {
        if (shape_addr == 0) return "null";
        if (shape_addr == rt.shapes.unit_shape) return "Unit";
        if (shape_addr == rt.shapes.symbol_shape) return "Symbol";
        if (shape_addr == rt.shapes.pointer_shape) return "Pointer";
        if (shape_addr == rt.shapes.shape_meta) return "Shape";
        if (shape_addr == rt.shapes.prototype_shape) return "Prototype";
        if (shape_addr == rt.shapes.handle_shape) return "Handle";
        const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(shape_addr);
        if (sl->is_leaf()) {
            return "bytes(" + std::to_string(sl->size) + ", " + std::to_string(sl->align) + ")";
        }
        std::string out = "struct{";
        const auto* entries = sl->fields.entries_begin();
        const uint32_t show = sl->fields.count < 4 ? sl->fields.count : 4;
        for (uint32_t i = 0; i < show; ++i) {
            if (i > 0) out += ", ";
            out += rt.symbols.name(entries[i].sym);
        }
        if (sl->fields.count > show) out += ", ...";
        out += "}";
        return out;
    };
    auto expect_shape = [&](const Runtime::Value& v, uint64_t expected, const char* msg) {
        if (v.shape_addr != expected) {
            throw std::runtime_error(std::string(msg) + " (got " + describe_shape(v.shape_addr) + ")");
        }
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
        case K::Parent: {
            expect_shape(args[0], rt.shapes.pointer_shape, "#parent: argument must be a Pointer");
            Runtime::ConstructingFrame* target = rt.find_open_frame(args[0].as_pod<Runtime::PointerLayout>());
            co_return rt.make_pointer(
                (target && target->parent) ? target->parent->frame_pointer() : Runtime::PointerLayout{});
        }
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
            co_return Runtime::Value::from_pod<uint8_t>(
                rt.shapes.raw1_shape, rt.coroutines.destroy(args[0].as_pod<uint64_t>()) ? 1 : 0);
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
            const auto ptr = args[0].as_pod<Runtime::PointerLayout>();
            if (ptr.is_null()) {
                if (const auto* fb = std::get_if<Ast::BuiltinExpr>(&b.args[0]->node);
                    fb && fb->kind == K::Field && fb->args.size() == 2) {
                    if (const auto* w = std::get_if<Ast::WordExpr>(&fb->args[1]->node)) {
                        throw std::runtime_error("#deref: null pointer (field '" + w->text + "' not found)");
                    }
                }
            }
            co_return rt.deref(ptr);
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
