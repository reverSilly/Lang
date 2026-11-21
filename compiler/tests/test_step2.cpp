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

    // Plain words (zero reserved words; '^' is now an ordinary Word char in Layer 0!)
    {
        ExprPtr e = fe.parse("  ! leading comment ! ^foo ! trailing !  ");
        auto* w = std::get_if<WordExpr>(&e->node);
        assert(w && w->text == "^foo");
    }

    // Full lexical escape for the 5 hardcoded symbols + whitespace + \! inside comment
    {
        ExprPtr e = fe.parse(R"(! comment with \! escaped bang ! \#not_builtin\;a\(b\)\ c\\)");
        auto* w = std::get_if<WordExpr>(&e->node);
        assert(w && w->text == "#not_builtin;a(b) c\\");
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
        ExprPtr e = fe.parse("#branch(cond, p1, p2)");
        auto* b = std::get_if<BuiltinExpr>(&e->node);
        assert(b && b->kind == BuiltinKind::Branch && b->args.size() == 3);
        assert(std::holds_alternative<WordExpr>(b->args[0]->node));
        assert(std::holds_alternative<WordExpr>(b->args[1]->node));
        assert(std::holds_alternative<WordExpr>(b->args[2]->node));
    }

    // Straight-line Layer 0 bootstrap sequence (no ^Expr needed!)
    {
        ExprPtr e = fe.parse(
            "#bind(text, #allocate_val(foo, is_mut)) ; "
            "#evaluate(#deref(#field(ast, word))) ; "
            "#drop(#field(#this, text))");
        auto* seq = std::get_if<SequenceExpr>(&e->node);
        assert(seq && seq->items.size() == 3);
        auto* b0 = std::get_if<BuiltinExpr>(&seq->items[0]->node);
        auto* b1 = std::get_if<BuiltinExpr>(&seq->items[1]->node);
        auto* b2 = std::get_if<BuiltinExpr>(&seq->items[2]->node);
        assert(b0 && b0->kind == BuiltinKind::Bind);
        assert(b1 && b1->kind == BuiltinKind::Evaluate);
        assert(b2 && b2->kind == BuiltinKind::Drop);
    }
}

static void test_grouping_and_sequences() {
    Frontend fe;

    // Pure grouping (no GroupExpr node in AST)
    {
        ExprPtr e = fe.parse("((a ; b))");
        auto* seq = std::get_if<SequenceExpr>(&e->node);
        assert(seq && seq->items.size() == 2);
        assert(std::get_if<WordExpr>(&seq->items[0]->node)->text == "a");
        assert(std::get_if<WordExpr>(&seq->items[1]->node)->text == "b");
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
    expect_parse_error("! unclosed");
    expect_parse_error("#compile(a)");     // #compile removed from Core
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

    // Dynamically extend Layer 0 Grammar with:
    // 1. Delimiter += . * +
    // 2. PostfixOp += .field
    // 3. Vertical wrap_rule: Mul ('*') tighter than Add ('+') on top of Infix
    fe.grammar.append_choice("Delimiter", char_set(".*+"));

    fe.register_postfix(
        "DotField",
        seq({ch('.'), fe.grammar.ref("_"), fe.grammar.ref("Word")}),
        [](ExprPtr lhs, const auto& ctx, const SyntaxNode& op_node) -> ExprPtr {
            ExprPtr sym = ctx.eval.evaluate(ctx.grammar, ctx.input, *op_node.children.at(0));
            return make_expr<BuiltinExpr>(BuiltinKind::Field, std::vector<ExprPtr>{std::move(lhs), std::move(sym)});
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
        assert(std::get_if<WordExpr>(&add->args[0]->node)->text == "a");
        auto* mul = std::get_if<BuiltinExpr>(&add->args[1]->node);
        assert(mul && mul->kind == BuiltinKind::Write && mul->args.size() == 2);
        assert(std::get_if<WordExpr>(&mul->args[0]->node)->text == "b");
        assert(std::get_if<WordExpr>(&mul->args[1]->node)->text == "c");
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
}

int main() {
    test_word_escapes_and_comments();
    test_builtins_and_arity();
    test_grouping_and_sequences();
    test_strict_non_nullable_and_errors();
    test_horizontal_and_vertical_layer1_extensions();
    std::cout << "All Step 2 tests passed!" << std::endl;
    return 0;
}
