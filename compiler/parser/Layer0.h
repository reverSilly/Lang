#pragma once

#include "../ast/Ast.h"
#include "Peg.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace reverSilly::Lang::Parser {

struct BuiltinSpec {
    Ast::BuiltinKind kind;
    size_t arity;
};

const std::unordered_map<std::string_view, BuiltinSpec>& builtin_table();
std::string unescape_word(std::string_view raw);

// Unified extensible Frontend:
// Layer 0 has 5 hardcoded symbols (\, !...!, #, (,), ;) and 21 symmetric Core builtins.
class Frontend {
public:
    Peg::Grammar grammar;
    Peg::Evaluator<Ast::ExprPtr> evaluator;

    using PostfixFolder = std::function<Ast::ExprPtr(
        Ast::ExprPtr, const Peg::Evaluator<Ast::ExprPtr>::Context&, const Peg::SyntaxNode&)>;
    std::vector<PostfixFolder> postfix_folders;

    Frontend();

    Ast::ExprPtr parse(std::string_view source) const;
    Peg::RuleId register_postfix(std::string_view rule_name, Peg::PegExprPtr expr, PostfixFolder folder);

private:
    void register_layer0();
};

} // namespace reverSilly::Lang::Parser
