#pragma once

#include "../ast/Ast.h"
#include "../parser/Layer0.h"
#include "Evaluator.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace reverSilly::Lang::Eval {

class CapabilityHost {
private:
    enum class HandleTag : uint8_t {
        Ast = 1,
        Peg = 2,
        Frontend = 3,
        Cst = 4,
    };

    std::vector<Ast::ExprPtr> ast_pool_;
    std::vector<Peg::PegExprPtr> peg_pool_;
    std::vector<std::unique_ptr<Parser::Frontend>> frontend_pool_;
    std::vector<const Peg::Evaluator<Ast::ExprPtr>::Context*> cst_pool_;
    // Arena owning host-allocated read-only byte buffers (e.g. io.read_file, cst_view_bytes).
    // unique_ptr<std::string> ensures stable data() pointers across vector growth; freed with CapabilityHost.
    std::vector<std::unique_ptr<std::string>> byte_buffers_;

    static uint64_t encode_handle(HandleTag tag, size_t idx) noexcept {
        return (static_cast<uint64_t>(tag) << 56) | static_cast<uint64_t>(idx);
    }

    static size_t decode_handle(uint64_t handle, HandleTag expected, size_t pool_size, const char* what) {
        uint8_t tag = static_cast<uint8_t>(handle >> 56);
        size_t idx = static_cast<size_t>(handle & 0x00FFFFFFFFFFFFFFULL);
        if (tag != static_cast<uint8_t>(expected) || idx >= pool_size) {
            throw std::runtime_error(std::string("capabilities: invalid or mismatched ") + what + " handle");
        }
        return idx;
    }

    static Runtime::Value read_field(Evaluator& ev, std::string_view name) {
        Runtime::ConstructingFrame* f = ev.rt.current_frame();
        Runtime::SymbolId sym = ev.rt.symbols.intern(name);
        Runtime::PointerLayout p = f->field(sym);
        if (p.is_null()) {
            throw std::runtime_error("capability missing required field '" + std::string(name) + "' in #this");
        }
        return ev.rt.deref(p);
    }

    static std::span<const uint8_t> read_bytes_ptr(Evaluator& ev, std::string_view name) {
        Runtime::Value v = read_field(ev, name);
        if (v.shape_addr != ev.rt.shapes.pointer_shape) {
            throw std::runtime_error("capabilities: field '" + std::string(name) + "' must be a Pointer to raw bytes");
        }
        auto p = v.as_pod<Runtime::PointerLayout>();
        if (p.shape_ptr == 0) {
            throw std::runtime_error("capabilities: null shape_ptr on byte pointer '" + std::string(name) + "'");
        }
        const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(p.shape_ptr);
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.addr), sl->size);
    }

    static std::vector<Runtime::Value> read_positional(Evaluator& ev) {
        Runtime::ConstructingFrame* f = ev.rt.current_frame();
        std::vector<Runtime::Value> out;
        for (size_t i = 0;; ++i) {
            Runtime::SymbolId sym = ev.rt.symbols.intern(std::to_string(i));
            Runtime::PointerLayout p = f->field(sym);
            if (p.is_null()) break;
            out.push_back(ev.rt.deref(p));
        }
        return out;
    }

    Runtime::Value store_bytes_ptr(Runtime::RuntimeContext& rt, std::string data) {
        auto buf = std::make_unique<std::string>(std::move(data));
        Runtime::Value ptr_val = rt.make_bytes_ptr(buf->data(), buf->size());
        byte_buffers_.push_back(std::move(buf));
        return ptr_val;
    }

    uint64_t store_ast(Ast::ExprPtr expr) {
        size_t idx = ast_pool_.size();
        ast_pool_.push_back(std::move(expr));
        return encode_handle(HandleTag::Ast, idx);
    }

    Ast::ExprPtr load_ast(uint64_t handle) const {
        return ast_pool_[decode_handle(handle, HandleTag::Ast, ast_pool_.size(), "Ast")];
    }

    uint64_t store_peg(Peg::PegExprPtr expr) {
        size_t idx = peg_pool_.size();
        peg_pool_.push_back(std::move(expr));
        return encode_handle(HandleTag::Peg, idx);
    }

    Peg::PegExprPtr load_peg(uint64_t handle) const {
        return peg_pool_[decode_handle(handle, HandleTag::Peg, peg_pool_.size(), "PegExpr")];
    }

    uint64_t store_frontend(std::unique_ptr<Parser::Frontend> fe) {
        size_t idx = frontend_pool_.size();
        frontend_pool_.push_back(std::move(fe));
        return encode_handle(HandleTag::Frontend, idx);
    }

    Parser::Frontend* load_frontend(uint64_t handle) const {
        return frontend_pool_[decode_handle(handle, HandleTag::Frontend, frontend_pool_.size(), "Frontend")].get();
    }

    const Peg::Evaluator<Ast::ExprPtr>::Context* load_cst(uint64_t handle) const {
        const auto* ctx = cst_pool_[decode_handle(handle, HandleTag::Cst, cst_pool_.size(), "CST")];
        if (!ctx) {
            throw std::runtime_error("capabilities.syntax: CST handle is no longer active");
        }
        return ctx;
    }

    static Runtime::Value build_struct(
        Runtime::RuntimeContext& rt,
        const std::vector<std::pair<std::string_view, Runtime::Value>>& fields) {
        Runtime::ConstructingFrame tmp;
        for (const auto& [name, val] : fields) {
            Runtime::FrameSlot* slot = tmp.allocate_val(val, false);
            tmp.bind(rt.symbols.intern(name), slot->pointer());
        }
        return tmp.close_and_flatten(rt.shapes);
    }

    Runtime::Value native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn) {
        uint64_t addr = rt.prototypes.create_native(std::move(name), std::move(fn));
        return rt.make_proto(addr);
    }

public:
    void install(Runtime::RuntimeContext& rt) {
        auto& s = rt.shapes;

        auto shape_val = [&](uint64_t addr) {
            return Runtime::Value::from_pod(s.shape_meta, *reinterpret_cast<const Runtime::ShapeLayout*>(addr));
        };

        // 1. capabilities.constants
        Runtime::Value constants_obj = build_struct(rt, {
            {"true", rt.make_bool(true)},
            {"false", rt.make_bool(false)},
            {"u64_zero", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 0ULL)},
            {"u64_one", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 1ULL)},
            {"u64_max", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, std::numeric_limits<uint64_t>::max())},
            {"unit_shape", shape_val(s.unit_shape)},
            {"raw1_shape", shape_val(s.raw1_shape)},
            {"raw4_shape", shape_val(s.raw4_shape)},
            {"raw8_shape", shape_val(s.raw8_shape)},
            {"symbol_shape", shape_val(s.symbol_shape)},
            {"boolean_shape", shape_val(s.boolean_shape)},
            {"pointer_shape", shape_val(s.pointer_shape)},
            {"field_entry_shape", shape_val(s.field_entry_shape)},
            {"field_map_shape", shape_val(s.field_map_shape)},
            {"shape_meta", shape_val(s.shape_meta)},
            {"prototype_shape", shape_val(s.prototype_shape)},
            {"handle_shape", shape_val(s.handle_shape)},
        });

        // 2. capabilities.field_map
        Runtime::Value field_map_obj = build_struct(rt, {
            {"empty", native_proto(rt, "capabilities.field_map.empty", [](Evaluator& ev) {
                return Runtime::Value::from_pod(ev.rt.shapes.field_map_shape, Runtime::FieldMapArena::empty());
            })},
            {"is_empty", native_proto(rt, "capabilities.field_map.is_empty", [](Evaluator& ev) {
                auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
                return ev.rt.make_bool(Runtime::FieldMapArena::is_empty(fm));
            })},
            {"count", native_proto(rt, "capabilities.field_map.count", [](Evaluator& ev) {
                auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
                return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, Runtime::FieldMapArena::count(fm));
            })},
            {"get", native_proto(rt, "capabilities.field_map.get", [](Evaluator& ev) {
                auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                return ev.rt.make_pointer(Runtime::FieldMapArena::get(fm, sym));
            })},
            {"at", native_proto(rt, "capabilities.field_map.at", [](Evaluator& ev) {
                auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
                auto idx = static_cast<uint32_t>(read_field(ev, "index").as_pod<uint64_t>());
                return Runtime::Value::from_pod(ev.rt.shapes.field_entry_shape, Runtime::FieldMapArena::at(fm, idx));
            })},
            {"insert", native_proto(rt, "capabilities.field_map.insert", [](Evaluator& ev) {
                auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                auto ptr = read_field(ev, "ptr").as_pod<Runtime::PointerLayout>();
                auto next = ev.rt.shapes.field_maps.insert(fm, sym, ptr);
                return Runtime::Value::from_pod(ev.rt.shapes.field_map_shape, next);
            })},
            {"ptr_shape", native_proto(rt, "capabilities.field_map.ptr_shape", [](Evaluator& ev) {
                auto p = read_field(ev, "ptr").as_pod<Runtime::PointerLayout>();
                if (p.shape_ptr == 0) {
                    throw std::runtime_error("capabilities.field_map.ptr_shape: null shape_ptr");
                }
                const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(p.shape_ptr);
                return Runtime::Value::from_pod(ev.rt.shapes.shape_meta, *sl);
            })},
        });

        // 3. capabilities.symbols
        Runtime::Value symbols_obj = build_struct(rt, {
            {"byte_len", native_proto(rt, "capabilities.symbols.byte_len", [](Evaluator& ev) {
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, ev.rt.symbols.name(sym).size());
            })},
            {"byte_at", native_proto(rt, "capabilities.symbols.byte_at", [](Evaluator& ev) {
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                auto idx = read_field(ev, "index").as_pod<uint64_t>();
                std::string_view sv = ev.rt.symbols.name(sym);
                if (idx >= sv.size()) throw std::out_of_range("capabilities.symbols.byte_at: out of range");
                return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, static_cast<uint8_t>(sv[idx]));
            })},
            {"to_bytes", native_proto(rt, "capabilities.symbols.to_bytes", [](Evaluator& ev) {
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                std::string_view sv = ev.rt.symbols.name(sym);
                return ev.rt.make_bytes_ptr(sv.data(), sv.size());
            })},
            {"intern_bytes", native_proto(rt, "capabilities.symbols.intern_bytes", [](Evaluator& ev) {
                auto span = read_bytes_ptr(ev, "bytes");
                return ev.rt.make_symbol(ev.rt.symbols.intern(span));
            })},
        });

        // 4. capabilities.intrinsics
        Runtime::Value intrinsics_obj = build_struct(rt, {
            {"add_i64", native_proto(rt, "capabilities.intrinsics.add_i64", [](Evaluator& ev) {
                return Runtime::Value::from_pod<int64_t>(
                    ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() + read_field(ev, "b").as_pod<int64_t>());
            })},
            {"sub_i64", native_proto(rt, "capabilities.intrinsics.sub_i64", [](Evaluator& ev) {
                return Runtime::Value::from_pod<int64_t>(
                    ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() - read_field(ev, "b").as_pod<int64_t>());
            })},
            {"mul_i64", native_proto(rt, "capabilities.intrinsics.mul_i64", [](Evaluator& ev) {
                return Runtime::Value::from_pod<int64_t>(
                    ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() * read_field(ev, "b").as_pod<int64_t>());
            })},
            {"lt_i64", native_proto(rt, "capabilities.intrinsics.lt_i64", [](Evaluator& ev) {
                return ev.rt.make_bool(read_field(ev, "a").as_pod<int64_t>() < read_field(ev, "b").as_pod<int64_t>());
            })},
            {"eq_i64", native_proto(rt, "capabilities.intrinsics.eq_i64", [](Evaluator& ev) {
                return ev.rt.make_bool(read_field(ev, "a").as_pod<int64_t>() == read_field(ev, "b").as_pod<int64_t>());
            })},
            {"eq_sym", native_proto(rt, "capabilities.intrinsics.eq_sym", [](Evaluator& ev) {
                return ev.rt.make_bool(
                    read_field(ev, "a").as_pod<Runtime::SymbolId>() == read_field(ev, "b").as_pod<Runtime::SymbolId>());
            })},
            {"eq_shape", native_proto(rt, "capabilities.intrinsics.eq_shape", [](Evaluator& ev) {
                auto a = read_field(ev, "a").as_pod<Runtime::ShapeLayout>();
                auto b = read_field(ev, "b").as_pod<Runtime::ShapeLayout>();
                return ev.rt.make_bool(ev.canonical_shape_addr(a) == ev.canonical_shape_addr(b));
            })},
            {"bytes_at", native_proto(rt, "capabilities.intrinsics.bytes_at", [](Evaluator& ev) {
                auto span = read_bytes_ptr(ev, "bytes");
                auto idx = read_field(ev, "index").as_pod<uint64_t>();
                if (idx >= span.size()) throw std::out_of_range("capabilities.intrinsics.bytes_at: out of range");
                return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, span[idx]);
            })},
            {"bytes_slice", native_proto(rt, "capabilities.intrinsics.bytes_slice", [](Evaluator& ev) {
                auto span = read_bytes_ptr(ev, "bytes");
                auto start = read_field(ev, "start").as_pod<uint64_t>();
                auto len = read_field(ev, "len").as_pod<uint64_t>();
                if (start > span.size() || start + len > span.size()) {
                    throw std::out_of_range("capabilities.intrinsics.bytes_slice: out of range");
                }
                return ev.rt.make_bytes_ptr(span.data() + start, len);
            })},
            {"u8_to_i64", native_proto(rt, "capabilities.intrinsics.u8_to_i64", [](Evaluator& ev) {
                int64_t v = static_cast<int64_t>(read_field(ev, "b").as_pod<uint8_t>());
                return Runtime::Value::from_pod<int64_t>(ev.rt.shapes.raw8_shape, v);
            })},
        });

        // 5. capabilities.ast
        Runtime::Value ast_obj = build_struct(rt, {
            {"word", native_proto(rt, "capabilities.ast.word", [this](Evaluator& ev) {
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                return ev.rt.make_handle(store_ast(Ast::make_expr<Ast::WordExpr>(std::string(ev.rt.symbols.name(sym)))));
            })},
            {"builtin", native_proto(rt, "capabilities.ast.builtin", [this](Evaluator& ev) {
                auto kind_sym = read_field(ev, "kind").as_pod<Runtime::SymbolId>();
                std::string_view name = ev.rt.symbols.name(kind_sym);
                const auto& tbl = Parser::builtin_table();
                auto it = tbl.find(name);
                if (it == tbl.end()) {
                    throw std::runtime_error("capabilities.ast.builtin: unknown builtin '" + std::string(name) + "'");
                }
                auto pos_vals = read_positional(ev);
                if (pos_vals.size() != it->second.arity) {
                    throw std::runtime_error("capabilities.ast.builtin: arity mismatch for '" + std::string(name) + "'");
                }
                std::vector<Ast::ExprPtr> args;
                args.reserve(pos_vals.size());
                for (const auto& v : pos_vals) args.push_back(load_ast(v.as_pod<uint64_t>()));
                return ev.rt.make_handle(store_ast(Ast::make_expr<Ast::BuiltinExpr>(it->second.kind, std::move(args))));
            })},
            {"seq", native_proto(rt, "capabilities.ast.seq", [this](Evaluator& ev) {
                auto pos_vals = read_positional(ev);
                if (pos_vals.empty()) throw std::runtime_error("capabilities.ast.seq: sequence must be non-empty");
                std::vector<Ast::ExprPtr> items;
                items.reserve(pos_vals.size());
                for (const auto& v : pos_vals) items.push_back(load_ast(v.as_pod<uint64_t>()));
                return ev.rt.make_handle(store_ast(Ast::make_expr<Ast::SequenceExpr>(std::move(items))));
            })},
            {"value", native_proto(rt, "capabilities.ast.value", [this](Evaluator& ev) {
                return ev.rt.make_handle(store_ast(Ast::make_expr<Ast::ValueExpr>(read_field(ev, "val"))));
            })},
            {"to_proto", native_proto(rt, "capabilities.ast.to_proto", [this](Evaluator& ev) {
                Ast::ExprPtr expr = load_ast(read_field(ev, "ast").as_pod<uint64_t>());
                return ev.rt.make_proto(ev.rt.prototypes.create_ast(std::move(expr)));
            })},
        });

        // 6. capabilities.syntax
        Runtime::Value syntax_obj = build_struct(rt, {
            {"new_layer0", native_proto(rt, "capabilities.syntax.new_layer0", [this](Evaluator& ev) {
                return ev.rt.make_handle(store_frontend(std::make_unique<Parser::Frontend>()));
            })},
            {"lit", native_proto(rt, "capabilities.syntax.lit", [this](Evaluator& ev) {
                auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
                return ev.rt.make_handle(store_peg(Peg::lit(std::string(ev.rt.symbols.name(sym)))));
            })},
            {"range", native_proto(rt, "capabilities.syntax.range", [this](Evaluator& ev) {
                return ev.rt.make_handle(
                    store_peg(Peg::range(read_field(ev, "lo").as_pod<uint8_t>(), read_field(ev, "hi").as_pod<uint8_t>())));
            })},
            {"ref", native_proto(rt, "capabilities.syntax.ref", [this](Evaluator& ev) {
                auto* fe = load_frontend(read_field(ev, "fe").as_pod<uint64_t>());
                auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
                return ev.rt.make_handle(store_peg(fe->grammar.ref(ev.rt.symbols.name(sym))));
            })},
            {"any", rt.make_handle(store_peg(Peg::any_byte()))},
            {"eof", rt.make_handle(store_peg(Peg::eof()))},
            {"seq", native_proto(rt, "capabilities.syntax.seq", [this](Evaluator& ev) {
                auto pos_vals = read_positional(ev);
                std::vector<Peg::PegExprPtr> items;
                items.reserve(pos_vals.size());
                for (const auto& v : pos_vals) items.push_back(load_peg(v.as_pod<uint64_t>()));
                return ev.rt.make_handle(store_peg(Peg::seq(std::move(items))));
            })},
            {"choice", native_proto(rt, "capabilities.syntax.choice", [this](Evaluator& ev) {
                auto pos_vals = read_positional(ev);
                std::vector<Peg::PegExprPtr> alts;
                alts.reserve(pos_vals.size());
                for (const auto& v : pos_vals) alts.push_back(load_peg(v.as_pod<uint64_t>()));
                return ev.rt.make_handle(store_peg(Peg::choice(std::move(alts))));
            })},
            {"repeat", native_proto(rt, "capabilities.syntax.repeat", [this](Evaluator& ev) {
                auto body = load_peg(read_field(ev, "body").as_pod<uint64_t>());
                auto mn = read_field(ev, "min").as_pod<uint64_t>();
                auto mx = read_field(ev, "max").as_pod<uint64_t>();
                return ev.rt.make_handle(store_peg(std::make_shared<Peg::PegExpr>(Peg::RepeatExpr{body, mn, mx})));
            })},
            {"lookahead", native_proto(rt, "capabilities.syntax.lookahead", [this](Evaluator& ev) {
                auto body = load_peg(read_field(ev, "body").as_pod<uint64_t>());
                bool positive = read_field(ev, "positive").as_pod<uint8_t>() != 0;
                return ev.rt.make_handle(store_peg(std::make_shared<Peg::PegExpr>(Peg::LookaheadExpr{body, positive})));
            })},
            {"get_rule", native_proto(rt, "capabilities.syntax.get_rule", [this](Evaluator& ev) {
                auto* fe = load_frontend(read_field(ev, "fe").as_pod<uint64_t>());
                auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
                Peg::PegExprPtr expr = fe->grammar.get_expr(ev.rt.symbols.name(sym));
                return ev.rt.make_handle(expr ? store_peg(std::move(expr)) : 0ULL);
            })},
            {"set_rule", native_proto(rt, "capabilities.syntax.set_rule", [this](Evaluator& ev) {
                auto* fe = load_frontend(read_field(ev, "fe").as_pod<uint64_t>());
                auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
                auto expr = load_peg(read_field(ev, "expr").as_pod<uint64_t>());
                bool hidden = read_field(ev, "hidden").as_pod<uint8_t>() != 0;
                uint64_t handler_proto = read_field(ev, "handler").as_pod<uint64_t>();

                Peg::RuleId id = fe->grammar.define(ev.rt.symbols.name(sym), std::move(expr), hidden);
                if (handler_proto != 0) {
                    Evaluator* ev_ptr = &ev;
                    fe->evaluator.on(id, [this, ev_ptr, handler_proto](const auto& ctx) -> Ast::ExprPtr {
                        size_t cst_idx = cst_pool_.size();
                        cst_pool_.push_back(&ctx);
                        uint64_t cst_h = encode_handle(HandleTag::Cst, cst_idx);

                        Runtime::ConstructingFrame* vf = ev_ptr->rt.active_coro->push_frame();
                        Runtime::FrameSlot* node_slot = vf->allocate_val(ev_ptr->rt.make_handle(cst_h), false);
                        vf->bind(ev_ptr->rt.symbols.intern("node"), node_slot->pointer());

                        Runtime::Value res_handle_val;
                        try {
                            res_handle_val = ev_ptr->eval_proto_sync(handler_proto);
                        } catch (...) {
                            ev_ptr->rt.active_coro->pop_frame();
                            cst_pool_[cst_idx] = nullptr;
                            throw;
                        }
                        ev_ptr->rt.active_coro->pop_frame();
                        cst_pool_[cst_idx] = nullptr;
                        return load_ast(res_handle_val.as_pod<uint64_t>());
                    });
                }
                return ev.rt.make_unit();
            })},
            {"cst_child_count", native_proto(rt, "capabilities.syntax.cst_child_count", [this](Evaluator& ev) {
                const auto* ctx = load_cst(read_field(ev, "node").as_pod<uint64_t>());
                return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, ctx->size());
            })},
            {"cst_view_sym", native_proto(rt, "capabilities.syntax.cst_view_sym", [this](Evaluator& ev) {
                const auto* ctx = load_cst(read_field(ev, "node").as_pod<uint64_t>());
                return ev.rt.make_symbol(ev.rt.symbols.intern(ctx->view()));
            })},
            {"cst_view_bytes", native_proto(rt, "capabilities.syntax.cst_view_bytes", [this](Evaluator& ev) {
                const auto* ctx = load_cst(read_field(ev, "node").as_pod<uint64_t>());
                return store_bytes_ptr(ev.rt, std::string(ctx->view()));
            })},
            {"cst_child_view_sym", native_proto(rt, "capabilities.syntax.cst_child_view_sym", [this](Evaluator& ev) {
                const auto* ctx = load_cst(read_field(ev, "node").as_pod<uint64_t>());
                auto idx = read_field(ev, "index").as_pod<uint64_t>();
                return ev.rt.make_symbol(ev.rt.symbols.intern(ctx->child_view(idx)));
            })},
            {"cst_eval_child", native_proto(rt, "capabilities.syntax.cst_eval_child", [this](Evaluator& ev) {
                const auto* ctx = load_cst(read_field(ev, "node").as_pod<uint64_t>());
                auto idx = read_field(ev, "index").as_pod<uint64_t>();
                return ev.rt.make_handle(store_ast(ctx->evaluate_child(idx)));
            })},
        });

        // 7. capabilities.compiler
        Runtime::Value compiler_obj = build_struct(rt, {
            {"parse", native_proto(rt, "capabilities.compiler.parse", [this](Evaluator& ev) {
                auto* fe = load_frontend(read_field(ev, "fe").as_pod<uint64_t>());
                auto span = read_bytes_ptr(ev, "src");
                std::string_view source(reinterpret_cast<const char*>(span.data()), span.size());
                Ast::ExprPtr root = fe->parse(source);
                return ev.rt.make_proto(ev.rt.prototypes.create_ast(std::move(root)));
            })},
        });

        // 8. capabilities.io (minimal compile-script file IO; replaced by FFI load_extern_symbol in Phase 2)
        Runtime::Value io_obj = build_struct(rt, {
            {"read_file", native_proto(rt, "capabilities.io.read_file", [this](Evaluator& ev) {
                auto path_span = read_bytes_ptr(ev, "path");
                std::string path(reinterpret_cast<const char*>(path_span.data()), path_span.size());
                std::ifstream ifs(path, std::ios::binary);
                if (!ifs) {
                    throw std::runtime_error("capabilities.io.read_file: failed to open '" + path + "'");
                }
                std::ostringstream oss;
                oss << ifs.rdbuf();
                return store_bytes_ptr(ev.rt, oss.str());
            })},
            {"write_file", native_proto(rt, "capabilities.io.write_file", [](Evaluator& ev) {
                auto path_span = read_bytes_ptr(ev, "path");
                auto data_span = read_bytes_ptr(ev, "bytes");
                std::string path(reinterpret_cast<const char*>(path_span.data()), path_span.size());
                std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    throw std::runtime_error("capabilities.io.write_file: failed to open '" + path + "'");
                }
                if (!data_span.empty()) {
                    ofs.write(reinterpret_cast<const char*>(data_span.data()), static_cast<std::streamsize>(data_span.size()));
                }
                if (!ofs) {
                    throw std::runtime_error("capabilities.io.write_file: write failed for '" + path + "'");
                }
                return ev.rt.make_unit();
            })},
        });

        Runtime::Value capabilities_obj = build_struct(rt, {
            {"constants", std::move(constants_obj)},
            {"field_map", std::move(field_map_obj)},
            {"symbols", std::move(symbols_obj)},
            {"intrinsics", std::move(intrinsics_obj)},
            {"ast", std::move(ast_obj)},
            {"syntax", std::move(syntax_obj)},
            {"compiler", std::move(compiler_obj)},
            {"io", std::move(io_obj)},
        });

        Runtime::ConstructingFrame* root = rt.current_frame();
        Runtime::FrameSlot* slot = root->allocate_val(capabilities_obj, false);
        root->bind(rt.symbols.intern("capabilities"), slot->pointer());
    }
};

} // namespace reverSilly::Lang::Eval
