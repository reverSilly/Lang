#include "Layer0.h"

#include <utility>

namespace reverSilly::Lang::Parser {

const std::unordered_map<std::string_view, BuiltinSpec>& builtin_table() {
    using K = Ast::BuiltinKind;
    static const std::unordered_map<std::string_view, BuiltinSpec> kTable = {
        // 4 nullary environment & coroutine cursors
        {"this", {K::This, 0}},
        {"parent", {K::Parent, 0}},
        {"caller", {K::Caller, 0}},
        {"in", {K::In, 0}},

        // 5 evaluation & coroutine lifecycle / reflection primitives
        {"evaluate", {K::Evaluate, 1}},
        {"block", {K::Block, 1}},
        {"new", {K::New, 1}},
        {"destroy", {K::Destroy, 1}},
        {"coro_root_frame", {K::CoroRootFrame, 1}},

        // 5 allocation & slot state-machine primitives
        {"allocate", {K::Allocate, 2}},
        {"allocate_val", {K::AllocateVal, 2}},
        {"touch", {K::Touch, 1}},
        {"bind", {K::Bind, 2}},
        {"drop", {K::Drop, 1}},

        // 2 raw bit read/write primitives
        {"deref", {K::Deref, 1}},
        {"write", {K::Write, 2}},

        // 3 navigation, reflection & freeze primitives
        {"field", {K::Field, 2}},
        {"shape", {K::Shape, 1}},
        {"freeze", {K::Freeze, 1}},

        // 3 control-flow & coroutine primitives
        {"branch", {K::Branch, 3}},
        {"break", {K::Break, 1}},
        {"resume", {K::Resume, 2}},
    };
    return kTable;
}

std::string unescape_word(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            out.push_back(raw[++i]);
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

Frontend::Frontend() {
    register_layer0();
}

Ast::ExprPtr Frontend::parse(std::string_view source) const {
    Peg::PackratParser parser(grammar, source);
    Peg::SyntaxNodePtr cst = parser.parse("Program");
    return evaluator.evaluate(grammar, source, *cst);
}

Peg::RuleId Frontend::register_postfix(std::string_view rule_name, Peg::PegExprPtr expr, PostfixFolder folder) {
    Peg::RuleId id = grammar.define(rule_name, std::move(expr));
    grammar.prepend_choice("PostfixOp", grammar.ref(id));
    if (id >= postfix_folders.size()) {
        postfix_folders.resize(static_cast<size_t>(id) + 1);
    }
    postfix_folders[id] = std::move(folder);
    return id;
}

void Frontend::register_layer0() {
    using namespace Peg;

    grammar.define("Escaped", seq({ch('\\'), any_byte()}), /*hidden=*/true, /*cacheable=*/false);
    grammar.define("WhiteSpace", char_set(" \t\r\n\f\v"), /*hidden=*/true, /*cacheable=*/false);
    grammar.define("Delimiter", char_set(" \t\r\n\f\v!#(),;"), /*hidden=*/true, /*cacheable=*/false);

    grammar.define(
        "Comment",
        seq({
            ch('!'),
            zero_or_more(choice({
                grammar.ref("Escaped"),
                seq({not_pred(ch('!')), any_byte()}),
            })),
            ch('!'),
        }),
        /*hidden=*/true,
        /*cacheable=*/false);

    grammar.define(
        "_",
        zero_or_more(choice({grammar.ref("WhiteSpace"), grammar.ref("Comment")})),
        /*hidden=*/true,
        /*cacheable=*/false);

    RuleId word_id = grammar.define(
        "Word",
        one_or_more(choice({
            grammar.ref("Escaped"),
            seq({not_pred(grammar.ref("Delimiter")), any_byte()}),
        })));

    grammar.define(
        "BuiltinArgs",
        seq({
            ch('('),
            grammar.ref("_"),
            grammar.ref("Expr"),
            zero_or_more(seq({grammar.ref("_"), ch(','), grammar.ref("_"), grammar.ref("Expr")})),
            grammar.ref("_"),
            ch(')'),
        }));

    RuleId builtin_id = grammar.define(
        "Builtin",
        seq({
            ch('#'),
            grammar.ref(word_id),
            opt(seq({grammar.ref("_"), grammar.ref("BuiltinArgs")})),
        }));

    grammar.define(
        "Group",
        seq({ch('('), grammar.ref("_"), grammar.ref("Expr"), grammar.ref("_"), ch(')')}));

    grammar.define("Atom", choice({grammar.ref(builtin_id), grammar.ref("Group"), grammar.ref(word_id)}));

    grammar.define("PostfixOp", fail_expr());
    RuleId postfix_id = grammar.define(
        "Postfix",
        seq({
            grammar.ref("Atom"),
            zero_or_more(seq({grammar.ref("_"), grammar.ref("PostfixOp")})),
        }));

    grammar.define("Prefix", grammar.ref(postfix_id));
    grammar.define("Infix", grammar.ref("Prefix"));

    RuleId seq_id = grammar.define(
        "Seq",
        seq({
            grammar.ref("Infix"),
            zero_or_more(seq({grammar.ref("_"), ch(';'), grammar.ref("_"), grammar.ref("Infix")})),
        }));

    grammar.define("Expr", grammar.ref(seq_id));
    grammar.define("Program", seq({grammar.ref("_"), grammar.ref("Expr"), grammar.ref("_"), eof()}));

    evaluator.on(word_id, [](const auto& ctx) -> Ast::ExprPtr {
        return Ast::make_expr<Ast::WordExpr>(unescape_word(ctx.view()));
    });

    evaluator.on(builtin_id, [](const auto& ctx) -> Ast::ExprPtr {
        std::string name = unescape_word(ctx.child_view(0));
        const auto& table = builtin_table();
        auto it = table.find(name);
        if (it == table.end()) {
            throw Peg::ParseError(ctx.location(), "unknown Core builtin '#" + name + "'");
        }
        std::vector<Ast::ExprPtr> args;
        if (ctx.size() == 2) {
            const Peg::SyntaxNode& args_node = ctx.child_node(1);
            args.reserve(args_node.children.size());
            for (const auto& arg_child : args_node.children) {
                args.push_back(ctx.eval.evaluate(ctx.grammar, ctx.input, *arg_child));
            }
        }
        if (args.size() != it->second.arity) {
            throw Peg::ParseError(
                ctx.location(),
                "builtin '#" + name + "' expects " + std::to_string(it->second.arity) +
                    " argument(s), got " + std::to_string(args.size()));
        }
        return Ast::make_expr<Ast::BuiltinExpr>(it->second.kind, std::move(args));
    });

    evaluator.on(postfix_id, [this](const auto& ctx) -> Ast::ExprPtr {
        Ast::ExprPtr acc = ctx.evaluate_child(0);
        for (size_t i = 1; i < ctx.size(); ++i) {
            const Peg::SyntaxNode& op_wrapper = ctx.child_node(i);
            const Peg::SyntaxNode& op_node = *op_wrapper.children.at(0);
            if (op_node.rule_id >= postfix_folders.size() || !postfix_folders[op_node.rule_id]) {
                throw Peg::ParseError(
                    ctx.location(),
                    "no postfix folder for '" + ctx.grammar.rule(op_node.rule_id).name + "'");
            }
            acc = postfix_folders[op_node.rule_id](std::move(acc), ctx, op_node);
        }
        return acc;
    });

    evaluator.on(seq_id, [](const auto& ctx) -> Ast::ExprPtr {
        if (ctx.size() == 1) {
            return ctx.evaluate_child(0);
        }
        std::vector<Ast::ExprPtr> items;
        items.reserve(ctx.size());
        for (size_t i = 0; i < ctx.size(); ++i) {
            items.push_back(ctx.evaluate_child(i));
        }
        return Ast::make_expr<Ast::SequenceExpr>(std::move(items));
    });
}

} // namespace reverSilly::Lang::Parser
