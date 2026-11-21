#include "../ast/Ast.h"
#include "../parser/Layer0.h"

#include <cassert>
#include <iostream>
#include <string>
#include <variant>

using namespace reverSilly::Lang::Ast;
using namespace reverSilly::Lang::Parser;
using namespace reverSilly::Lang::Peg;

static void test_word_escapes_and_comments() {
    Frontend fe;

    {
        ExprPtr e = fe.parse("  ! leading comment ! if ! trailing !  ");
        auto* w = std::get_if<WordExpr>(&e->node);
        assert(w && w->text == "if");
    }

    {
        ExprPtr e = fe.parse(R"(! comment with \! escaped bang ! \#not_builtin\^a\;b\ c\\)");
        auto* w = std::get_if<WordExpr>(&e->node);
        assert(w && w->text == "#not_builtin^a;b c\\");
    }
}

static void test_builtins_and_arity() {
    Frontend fe;

    const std::pair<const char*, BuiltinKind> nullaries[] = {
        {"#this", BuiltinKind::This},
        {"#parent", BuiltinKind::Parent},
        {"#unbound", BuiltinKind::Unbound},
        {"#caller", BuiltinKind::Caller},
        {"#in", BuiltinKind::In},
    };
    for (auto [src, expected] : nullaries) {
        ExprPtr e = fe.parse(src);
        auto* b = std::get_if<BuiltinExpr>(&e->node);
        assert(b && b->kind == expected && b->args.empty());
    }

    {
        ExprPtr e = fe.parse("#branch(cond, ^a, ^b)");
        auto* b = std::get_if<BuiltinExpr>(&e->node);
        assert(b && b->kind == BuiltinKind::Branch && b->args.size() == 3);
        assert(std::holds_alternative<WordExpr>(b->args[0]->node));
        assert(std::holds_alternative<PrototypeExpr>(b->args[1]->node));
        assert(std::holds_alternative<PrototypeExpr>(b->args[2]->node));
    }

    {
        ExprPtr e = fe.parse("#block(^#bind(x, #allocate_val(foo, bar)))");
        auto* blk = std::get_if<BuiltinExpr>(&e->node);
        assert(blk && blk->kind == BuiltinKind::Block && blk->args.size() == 1);
        auto* proto = std::get_if<PrototypeExpr>(&blk->args[0]->node);
        assert(proto);
        auto* bind = std::get_if<BuiltinExpr>(&proto->body->node);
        assert(bind && bind->kind == BuiltinKind::Bind && bind->args.size() == 2);
        auto* sym = std::get_if<WordExpr>(&bind->args[0]->node);
        assert(sym && sym->text == "x");
        auto* alloc = std::get_if<BuiltinExpr>(&bind->args[1]->node);
        assert(alloc && alloc->kind == BuiltinKind::AllocateVal && alloc->args.size() == 2);
    }
}

static void test_precedence_grouping_and_sequences() {
    Frontend fe;

    {
        ExprPtr e = fe.parse("^^x");
        auto* p1 = std::get_if<PrototypeExpr>(&e->node);
        assert(p1);
        auto* p2 = std::get_if<PrototypeExpr>(&p1->body->node);
        assert(p2);
        auto* w = std::get_if<WordExpr>(&p2->body->node);
        assert(w && w->text == "x");
    }

    {
        ExprPtr e = fe.parse("^a ; b");
        auto* seq = std::get_if<SequenceExpr>(&e->node);
        assert(seq && seq->items.size() == 2);
        assert(std::holds_alternative<PrototypeExpr>(seq->items[0]->node));
        assert(std::holds_alternative<WordExpr>(seq->items[1]->node));
    }

    {
        ExprPtr e = fe.parse("^(a ; b)");
        auto* p = std::get_if<PrototypeExpr>(&e->node);
        assert(p);
        auto* seq = std::get_if<SequenceExpr>(&p->body->node);
        assert(seq && seq->items.size() == 2);
    }
}

static void test_strict_non_nullable_and_errors() {
    Frontend fe;

    auto expect_parse_error = [&](const char* src) {
        bool threw = false;
        try {
            (void)fe.parse(src);
        } catch (const ParseError&) {
            threw = true;
        }
        assert(threw);
    };

    expect_parse_error("");
    expect_parse_error("   ! only comment !");
    expect_parse_error("()");
    expect_parse_error("a ;");
    expect_parse_error("a ; ; b");
    expect_parse_error("^");
    expect_parse_error("! unclosed");
    expect_parse_error("#unknown_builtin");
    expect_parse_error("#this(a)");
    expect_parse_error("#bind(x)");
    expect_parse_error("#branch(a, b)");

    {
        Grammar bad;
        bad.define("Loop", seq({bad.ref("Loop"), ch('a')}));
        PackratParser p(bad, "aaa");
        bool threw = false;
        try {
            (void)p.parse("Loop");
        } catch (const ParseError&) {
            threw = true;
        }
        assert(threw);
    }
}

static void test_horizontal_and_vertical_layer1_extensions() {
    Frontend fe;

    fe.grammar.append_choice("Delimiter", char_set(".{}*+"));

    fe.register_postfix(
        "DotField",
        seq({ch('.'), fe.grammar.ref("_"), fe.grammar.ref("Word")}),
        [](ExprPtr lhs, const auto& ctx, const SyntaxNode& op_node) -> ExprPtr {
            ExprPtr sym = ctx.eval.evaluate(ctx.grammar, ctx.input, *op_node.children.at(0));
            return make_expr<BuiltinExpr>(BuiltinKind::Field, std::vector<ExprPtr>{std::move(lhs), std::move(sym)});
        });

    RuleId empty_blk_id = fe.grammar.define("EmptyBlock", seq({ch('{'), fe.grammar.ref("_"), ch('}')}));
    fe.grammar.prepend_choice("Atom", fe.grammar.ref(empty_blk_id));
    fe.evaluator.on(empty_blk_id, [](const auto&) -> ExprPtr {
        auto this_expr = make_expr<BuiltinExpr>(BuiltinKind::This, std::vector<ExprPtr>{});
        auto proto = make_expr<PrototypeExpr>(this_expr);
        return make_expr<BuiltinExpr>(BuiltinKind::Block, std::vector<ExprPtr>{proto});
    });

    RuleId mul_id = fe.grammar.wrap_rule("Infix", "Mul", [&](PegExprPtr inner) {
        return seq({inner, zero_or_more(seq({fe.grammar.ref("_"), ch('*'), fe.grammar.ref("_"), inner}))});
    });
    fe.evaluator.on(mul_id, [](const auto& ctx) -> ExprPtr {
        ExprPtr acc = ctx.evaluate_child(0);
        for (size_t i = 1; i < ctx.size(); ++i) {
            acc = make_expr<BuiltinExpr>(
                BuiltinKind::Write, std::vector<ExprPtr>{std::move(acc), ctx.evaluate_child(i)});
        }
        return acc;
    });

    RuleId add_id = fe.grammar.wrap_rule("Infix", "Add", [&](PegExprPtr inner) {
        return seq({inner, zero_or_more(seq({fe.grammar.ref("_"), ch('+'), fe.grammar.ref("_"), inner}))});
    });
    fe.evaluator.on(add_id, [](const auto& ctx) -> ExprPtr {
        ExprPtr acc = ctx.evaluate_child(0);
        for (size_t i = 1; i < ctx.size(); ++i) {
            acc = make_expr<BuiltinExpr>(
                BuiltinKind::Bind, std::vector<ExprPtr>{std::move(acc), ctx.evaluate_child(i)});
        }
        return acc;
    });

    {
        ExprPtr e = fe.parse("a + b * c");
        auto* add = std::get_if<BuiltinExpr>(&e->node);
        assert(add && add->kind == BuiltinKind::Bind && add->args.size() == 2);
        auto* wa = std::get_if<WordExpr>(&add->args[0]->node);
        assert(wa && wa->text == "a");

        auto* mul = std::get_if<BuiltinExpr>(&add->args[1]->node);
        assert(mul && mul->kind == BuiltinKind::Write && mul->args.size() == 2);
        auto* wb = std::get_if<WordExpr>(&mul->args[0]->node);
        auto* wc = std::get_if<WordExpr>(&mul->args[1]->node);
        assert(wb && wb->text == "b");
        assert(wc && wc->text == "c");
    }

    {
        ExprPtr e = fe.parse("#this.x * y + z");
        auto* add = std::get_if<BuiltinExpr>(&e->node);
        assert(add && add->kind == BuiltinKind::Bind);
        auto* mul = std::get_if<BuiltinExpr>(&add->args[0]->node);
        assert(mul && mul->kind == BuiltinKind::Write);
        auto* fld = std::get_if<BuiltinExpr>(&mul->args[0]->node);
        assert(fld && fld->kind == BuiltinKind::Field);
    }

    {
        ExprPtr e = fe.parse(R"(foo\.bar\+baz)");
        auto* w = std::get_if<WordExpr>(&e->node);
        assert(w && w->text == "foo.bar+baz");
    }

    {
        ExprPtr e = fe.parse("{}");
        auto* blk = std::get_if<BuiltinExpr>(&e->node);
        assert(blk && blk->kind == BuiltinKind::Block && blk->args.size() == 1);
    }
}

int main() {
    test_word_escapes_and_comments();
    test_builtins_and_arity();
    test_precedence_grouping_and_sequences();
    test_strict_non_nullable_and_errors();
    test_horizontal_and_vertical_layer1_extensions();
    std::cout << "All Step 2 tests passed!" << std::endl;
    return 0;
}
