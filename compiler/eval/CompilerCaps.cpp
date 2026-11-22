#include "Capabilities.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace reverSilly::Lang::Eval {

Runtime::Value CapabilityHost::build_ast_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
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
}

Runtime::Value CapabilityHost::build_syntax_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
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
        {"parse", native_proto(rt, "capabilities.syntax.parse", [this](Evaluator& ev) {
            auto* fe = load_frontend(read_field(ev, "fe").as_pod<uint64_t>());
            auto span = read_bytes_ptr(ev, "src");
            std::string_view source(reinterpret_cast<const char*>(span.data()), span.size());
            Ast::ExprPtr root = fe->parse(source);
            return ev.rt.make_handle(store_ast(std::move(root)));
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
}

Runtime::Value CapabilityHost::build_compiler_module(Runtime::RuntimeContext& rt) {
    // Reserved for Prototype-level backend operations (serialize_proto, deserialize_proto, emit_executable).
    return build_struct(rt, {});
}

Runtime::Value CapabilityHost::build_io_module(Runtime::RuntimeContext& rt) {
    // Temporary Phase 1 compile-script file IO (to be superseded by capabilities.ffi).
    return build_struct(rt, {
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
}

} // namespace reverSilly::Lang::Eval
