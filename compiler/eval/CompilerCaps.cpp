#include "Capabilities.h"

#include "../ast/Ast.h"
#include "../parser/Layer0.h"

#include <deque>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace reverSilly::Lang::Eval::CapabilitiesDetail {

namespace {

// Explicit AST handle arena owned by Lang via ast.new_arena / ast.destroy_arena.
// std::deque guarantees pointer stability on emplace_back.
struct AstArena {
    std::deque<Ast::ExprPtr> nodes;

    uint64_t alloc(Ast::ExprPtr expr) {
        nodes.push_back(std::move(expr));
        return reinterpret_cast<uint64_t>(&nodes.back());
    }
};

// Explicit Frontend + PEG handle arena owned by Lang via syntax.new_layer0 / syntax.destroy_frontend.
struct FrontendArena {
    Parser::Frontend frontend;
    std::deque<Peg::PegExprPtr> peg_nodes;
    AstArena* active_parse_arena = nullptr;

    uint64_t alloc_peg(Peg::PegExprPtr expr) {
        peg_nodes.push_back(std::move(expr));
        return reinterpret_cast<uint64_t>(&peg_nodes.back());
    }
};

AstArena* load_ast_arena(Evaluator& ev) {
    uint64_t raw = read_field(ev, "arena").as_pod<uint64_t>();
    if (raw == 0) {
        throw std::runtime_error("capabilities.ast: null arena handle");
    }
    return reinterpret_cast<AstArena*>(raw);
}

Ast::ExprPtr load_ast_node(uint64_t handle) {
    if (handle == 0) {
        throw std::runtime_error("capabilities.ast: null AST handle");
    }
    return *reinterpret_cast<const Ast::ExprPtr*>(handle);
}

FrontendArena* load_frontend_arena(Evaluator& ev) {
    uint64_t raw = read_field(ev, "fe").as_pod<uint64_t>();
    if (raw == 0) {
        throw std::runtime_error("capabilities.syntax: null frontend handle");
    }
    return reinterpret_cast<FrontendArena*>(raw);
}

Peg::PegExprPtr load_peg_node(uint64_t handle) {
    if (handle == 0) {
        throw std::runtime_error("capabilities.syntax: null PEG handle");
    }
    return *reinterpret_cast<const Peg::PegExprPtr*>(handle);
}

const Peg::Evaluator<Ast::ExprPtr>::Context* load_cst_ctx(Evaluator& ev) {
    uint64_t raw = read_field(ev, "node").as_pod<uint64_t>();
    if (raw == 0) {
        throw std::runtime_error("capabilities.syntax: null CST node handle");
    }
    return reinterpret_cast<const Peg::Evaluator<Ast::ExprPtr>::Context*>(raw);
}

uint64_t static_any_peg_handle() {
    static const Peg::PegExprPtr kAny = Peg::any_byte();
    return reinterpret_cast<uint64_t>(&kAny);
}

uint64_t static_eof_peg_handle() {
    static const Peg::PegExprPtr kEof = Peg::eof();
    return reinterpret_cast<uint64_t>(&kEof);
}

} // namespace

Runtime::Value build_ast_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"new_arena", native_proto(rt, "capabilities.ast.new_arena", [](Evaluator& ev) {
            auto* arena = new AstArena();
            return ev.rt.make_handle(reinterpret_cast<uint64_t>(arena));
        })},
        {"destroy_arena", native_proto(rt, "capabilities.ast.destroy_arena", [](Evaluator& ev) {
            uint64_t raw = read_field(ev, "arena").as_pod<uint64_t>();
            delete reinterpret_cast<AstArena*>(raw);
            return ev.rt.make_unit();
        })},
        {"word", native_proto(rt, "capabilities.ast.word", [](Evaluator& ev) {
            AstArena* arena = load_ast_arena(ev);
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            return ev.rt.make_handle(arena->alloc(Ast::make_expr<Ast::WordExpr>(std::string(ev.rt.symbols.name(sym)))));
        })},
        {"builtin", native_proto(rt, "capabilities.ast.builtin", [](Evaluator& ev) {
            AstArena* arena = load_ast_arena(ev);
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
            for (const auto& v : pos_vals) args.push_back(load_ast_node(v.as_pod<uint64_t>()));
            return ev.rt.make_handle(arena->alloc(Ast::make_expr<Ast::BuiltinExpr>(it->second.kind, std::move(args))));
        })},
        {"seq", native_proto(rt, "capabilities.ast.seq", [](Evaluator& ev) {
            AstArena* arena = load_ast_arena(ev);
            auto pos_vals = read_positional(ev);
            if (pos_vals.empty()) throw std::runtime_error("capabilities.ast.seq: sequence must be non-empty");
            std::vector<Ast::ExprPtr> items;
            items.reserve(pos_vals.size());
            for (const auto& v : pos_vals) items.push_back(load_ast_node(v.as_pod<uint64_t>()));
            return ev.rt.make_handle(arena->alloc(Ast::make_expr<Ast::SequenceExpr>(std::move(items))));
        })},
        {"value", native_proto(rt, "capabilities.ast.value", [](Evaluator& ev) {
            AstArena* arena = load_ast_arena(ev);
            return ev.rt.make_handle(arena->alloc(Ast::make_expr<Ast::ValueExpr>(reify_ast_value(ev.rt, read_field(ev, "val")))));
        })},
        {"to_proto", native_proto(rt, "capabilities.ast.to_proto", [](Evaluator& ev) {
            Ast::ExprPtr expr = load_ast_node(read_field(ev, "ast").as_pod<uint64_t>());
            return ev.rt.make_proto(ev.rt.prototypes.create_ast(std::move(expr)));
        })},
    });
}

Runtime::Value build_syntax_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"new_layer0", native_proto(rt, "capabilities.syntax.new_layer0", [](Evaluator& ev) {
            auto* fe = new FrontendArena();
            return ev.rt.make_handle(reinterpret_cast<uint64_t>(fe));
        })},
        {"destroy_frontend", native_proto(rt, "capabilities.syntax.destroy_frontend", [](Evaluator& ev) {
            uint64_t raw = read_field(ev, "fe").as_pod<uint64_t>();
            delete reinterpret_cast<FrontendArena*>(raw);
            return ev.rt.make_unit();
        })},
        {"lit", native_proto(rt, "capabilities.syntax.lit", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            return ev.rt.make_handle(fe->alloc_peg(Peg::lit(std::string(ev.rt.symbols.name(sym)))));
        })},
        {"range", native_proto(rt, "capabilities.syntax.range", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            return ev.rt.make_handle(
                fe->alloc_peg(Peg::range(read_field(ev, "lo").as_pod<uint8_t>(), read_field(ev, "hi").as_pod<uint8_t>())));
        })},
        {"ref", native_proto(rt, "capabilities.syntax.ref", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
            return ev.rt.make_handle(fe->alloc_peg(fe->frontend.grammar.ref(ev.rt.symbols.name(sym))));
        })},
        {"any", rt.make_handle(static_any_peg_handle())},
        {"eof", rt.make_handle(static_eof_peg_handle())},
        {"seq", native_proto(rt, "capabilities.syntax.seq", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto pos_vals = read_positional(ev);
            std::vector<Peg::PegExprPtr> items;
            items.reserve(pos_vals.size());
            for (const auto& v : pos_vals) items.push_back(load_peg_node(v.as_pod<uint64_t>()));
            return ev.rt.make_handle(fe->alloc_peg(Peg::seq(std::move(items))));
        })},
        {"choice", native_proto(rt, "capabilities.syntax.choice", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto pos_vals = read_positional(ev);
            std::vector<Peg::PegExprPtr> alts;
            alts.reserve(pos_vals.size());
            for (const auto& v : pos_vals) alts.push_back(load_peg_node(v.as_pod<uint64_t>()));
            return ev.rt.make_handle(fe->alloc_peg(Peg::choice(std::move(alts))));
        })},
        {"repeat", native_proto(rt, "capabilities.syntax.repeat", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto body = load_peg_node(read_field(ev, "body").as_pod<uint64_t>());
            auto mn = read_field(ev, "min").as_pod<uint64_t>();
            auto mx = read_field(ev, "max").as_pod<uint64_t>();
            return ev.rt.make_handle(fe->alloc_peg(std::make_shared<Peg::PegExpr>(Peg::RepeatExpr{body, mn, mx})));
        })},
        {"lookahead", native_proto(rt, "capabilities.syntax.lookahead", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto body = load_peg_node(read_field(ev, "body").as_pod<uint64_t>());
            bool positive = read_field(ev, "positive").as_pod<uint8_t>() != 0;
            return ev.rt.make_handle(fe->alloc_peg(std::make_shared<Peg::PegExpr>(Peg::LookaheadExpr{body, positive})));
        })},
        {"get_rule", native_proto(rt, "capabilities.syntax.get_rule", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
            Peg::PegExprPtr expr = fe->frontend.grammar.get_expr(ev.rt.symbols.name(sym));
            return ev.rt.make_handle(expr ? fe->alloc_peg(std::move(expr)) : 0ULL);
        })},
        {"set_rule", native_proto(rt, "capabilities.syntax.set_rule", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            auto sym = read_field(ev, "name").as_pod<Runtime::SymbolId>();
            auto expr = load_peg_node(read_field(ev, "expr").as_pod<uint64_t>());
            bool hidden = read_field(ev, "hidden").as_pod<uint8_t>() != 0;
            uint64_t handler_proto = read_field(ev, "handler").as_pod<uint64_t>();

            Peg::RuleId id = fe->frontend.grammar.define(ev.rt.symbols.name(sym), std::move(expr), hidden);
            if (handler_proto != 0) {
                Evaluator* ev_ptr = &ev;
                fe->frontend.evaluator.on(id, [fe, ev_ptr, handler_proto](const auto& ctx) -> Ast::ExprPtr {
                    uint64_t cst_h = reinterpret_cast<uint64_t>(&ctx);
                    uint64_t arena_h = reinterpret_cast<uint64_t>(fe->active_parse_arena);

                    Runtime::ConstructingFrame* vf = ev_ptr->rt.active_coro->push_frame();
                    Runtime::FrameSlot* node_slot = vf->allocate_val(ev_ptr->rt.make_handle(cst_h), false);
                    vf->bind(ev_ptr->rt.symbols.intern("node"), node_slot->pointer());
                    Runtime::FrameSlot* arena_slot = vf->allocate_val(ev_ptr->rt.make_handle(arena_h), false);
                    vf->bind(ev_ptr->rt.symbols.intern("arena"), arena_slot->pointer());
                    Runtime::SymbolId caps_sym = ev_ptr->rt.symbols.intern("capabilities");
                    Runtime::PointerLayout root_caps = ev_ptr->rt.active_coro->root_frame()->field(caps_sym);
                    if (!root_caps.is_null()) {
                        Runtime::FrameSlot* caps_slot = vf->allocate_val(ev_ptr->rt.deref(root_caps), false);
                        vf->bind(caps_sym, caps_slot->pointer());
                    }

                    Runtime::Value res_handle_val;
                    try {
                        res_handle_val = ev_ptr->eval_proto_sync(handler_proto);
                    } catch (...) {
                        ev_ptr->rt.active_coro->pop_frame();
                        throw;
                    }
                    ev_ptr->rt.active_coro->pop_frame();
                    return load_ast_node(res_handle_val.as_pod<uint64_t>());
                });
            }
            return ev.rt.make_unit();
        })},
        {"parse", native_proto(rt, "capabilities.syntax.parse", [](Evaluator& ev) {
            FrontendArena* fe = load_frontend_arena(ev);
            AstArena* arena = load_ast_arena(ev);
            auto raw = read_raw_bytes(ev, "src");
            std::string_view source(reinterpret_cast<const char*>(raw.data()), raw.size());

            AstArena* prev_arena = fe->active_parse_arena;
            fe->active_parse_arena = arena;
            Ast::ExprPtr root;
            try {
                root = fe->frontend.parse(source);
            } catch (...) {
                fe->active_parse_arena = prev_arena;
                throw;
            }
            fe->active_parse_arena = prev_arena;
            return ev.rt.make_handle(arena->alloc(std::move(root)));
        })},
        {"cst_child_count", native_proto(rt, "capabilities.syntax.cst_child_count", [](Evaluator& ev) {
            const auto* ctx = load_cst_ctx(ev);
            return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, ctx->size());
        })},
        {"cst_view_sym", native_proto(rt, "capabilities.syntax.cst_view_sym", [](Evaluator& ev) {
            const auto* ctx = load_cst_ctx(ev);
            return ev.rt.make_symbol(ev.rt.symbols.intern(ctx->view()));
        })},
        {"cst_view_bytes", native_proto(rt, "capabilities.syntax.cst_view_bytes", [](Evaluator& ev) {
            const auto* ctx = load_cst_ctx(ev);
            return ev.rt.make_raw_bytes(ctx->view());
        })},
        {"cst_child_view_sym", native_proto(rt, "capabilities.syntax.cst_child_view_sym", [](Evaluator& ev) {
            const auto* ctx = load_cst_ctx(ev);
            auto idx = read_field(ev, "index").as_pod<uint64_t>();
            return ev.rt.make_symbol(ev.rt.symbols.intern(ctx->child_view(idx)));
        })},
        {"cst_eval_child", native_proto(rt, "capabilities.syntax.cst_eval_child", [](Evaluator& ev) {
            const auto* ctx = load_cst_ctx(ev);
            AstArena* arena = load_ast_arena(ev);
            auto idx = read_field(ev, "index").as_pod<uint64_t>();
            return ev.rt.make_handle(arena->alloc(ctx->evaluate_child(idx)));
        })},
    });
}

Runtime::Value build_backends_module(Runtime::RuntimeContext& rt) {
    // Target backends consume compile-time Core IR (`Ast` handle), never host `Prototype` objects.
    Runtime::Value interpreter_mod = build_struct(rt, {
        {"run", native_proto(rt, "capabilities.backends.interpreter.run", [](Evaluator& ev) {
            Ast::ExprPtr expr = load_ast_node(read_field(ev, "ast").as_pod<uint64_t>());
            Runtime::RuntimeContext target_rt;
            Evaluator target_ev(target_rt);
            Runtime::Value target_res = target_ev.run(expr);
            return intern_ast_value(ev.rt, reify_ast_value(target_rt, target_res));
        })},
    });
    return build_struct(rt, {
        {"interpreter", std::move(interpreter_mod)},
    });
}

Runtime::Value build_io_module(Runtime::RuntimeContext& rt) {
    // Temporary Phase 1 compile-script file IO (to be superseded by capabilities.ffi).
    return build_struct(rt, {
        {"read_file", native_proto(rt, "capabilities.io.read_file", [](Evaluator& ev) {
            auto path_raw = read_raw_bytes(ev, "path");
            std::string path(reinterpret_cast<const char*>(path_raw.data()), path_raw.size());
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) {
                throw std::runtime_error("capabilities.io.read_file: failed to open '" + path + "'");
            }
            std::ostringstream oss;
            oss << ifs.rdbuf();
            return ev.rt.make_raw_bytes(oss.str());
        })},
        {"write_file", native_proto(rt, "capabilities.io.write_file", [](Evaluator& ev) {
            auto path_raw = read_raw_bytes(ev, "path");
            auto data_raw = read_raw_bytes(ev, "bytes");
            std::string path(reinterpret_cast<const char*>(path_raw.data()), path_raw.size());
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                throw std::runtime_error("capabilities.io.write_file: failed to open '" + path + "'");
            }
            if (!data_raw.empty()) {
                ofs.write(reinterpret_cast<const char*>(data_raw.data()), static_cast<std::streamsize>(data_raw.size()));
            }
            if (!ofs) {
                throw std::runtime_error("capabilities.io.write_file: write failed for '" + path + "'");
            }
            return ev.rt.make_unit();
        })},
    });
}

} // namespace reverSilly::Lang::Eval::CapabilitiesDetail
