#pragma once

#include "../ast/Ast.h"
#include "../parser/Layer0.h"
#include "Evaluator.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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

        // 1. capabilities.constants (all 13 bootstrapped base Shapes + bootstrap scalars)
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
            {"bytes_shape", shape_val(rt.bytes_shape)},
            {"field_entry_shape", shape_val(s.field_entry_shape)},
            {"field_map_shape", shape_val(s.field_map_shape)},
            {"shape_meta", shape_val(s.shape_meta)},
            {"prototype_shape", shape_val(s.prototype_shape)},
            {"handle_shape", shape_val(s.handle_shape)},
        });

        // 2. capabilities.field_map (works uniformly on both closed Shapes and open frames' live_shape!)
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
                Runtime::BytesLayout bl{reinterpret_cast<uint64_t>(sv.data()), sv.size()};
                return ev.rt.make_bytes(bl);
            })},
        });

        // 4. capabilities.intrinsics (destroy_coro promoted to Core #destroy)
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
                Runtime::Value src_val = read_field(ev, "src");
                if (src_val.shape_addr != ev.rt.bytes_shape) {
                    throw std::runtime_error("capabilities.compiler.parse: 'src' must be Bytes");
                }
                auto bl = src_val.as_pod<Runtime::BytesLayout>();
                std::string_view source(reinterpret_cast<const char*>(bl.ptr), bl.len);
                Ast::ExprPtr root = fe->parse(source);
                return ev.rt.make_proto(ev.rt.prototypes.create_ast(std::move(root)));
            })},
        });

        // 8. capabilities.io
        Runtime::Value io_obj = build_struct(rt, {
            {"print_i64", native_proto(rt, "capabilities.io.print_i64", [](Evaluator& ev) {
                std::cout << read_field(ev, "val").as_pod<int64_t>() << '\n';
                return ev.rt.make_unit();
            })},
            {"print_sym", native_proto(rt, "capabilities.io.print_sym", [](Evaluator& ev) {
                std::cout << ev.rt.symbols.name(read_field(ev, "sym").as_pod<Runtime::SymbolId>()) << '\n';
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
