#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace reverSilly::Lang::Peg {

using RuleId = uint32_t;

struct SyntaxNode;
using SyntaxNodePtr = std::shared_ptr<SyntaxNode>;

struct SyntaxNode {
    RuleId rule_id = 0;
    size_t begin = 0;
    size_t end = 0;
    std::vector<SyntaxNodePtr> children;

    size_t length() const noexcept { return end - begin; }

    std::string_view view(std::string_view input) const noexcept {
        return input.substr(begin, end - begin);
    }
};

struct SourceLocation {
    size_t offset = 0;
    size_t line = 1;
    size_t col = 1;
};

SourceLocation compute_location(std::string_view input, size_t offset) noexcept;

class ParseError : public std::runtime_error {
public:
    SourceLocation loc;

    ParseError(SourceLocation l, const std::string& msg)
        : std::runtime_error(
              "ParseError at " + std::to_string(l.line) + ":" + std::to_string(l.col) + ": " + msg),
          loc(l) {}
};

// -----------------------------------------------------------------------------
// PEG Expression AST (std::variant, zero raw unions, cycle-free via RuleId)
// -----------------------------------------------------------------------------
struct PegExpr;
using PegExprPtr = std::shared_ptr<PegExpr>;

struct LitExpr { std::string text; };
struct RangeExpr { uint8_t lo; uint8_t hi; };
struct CharSetExpr { std::string chars; };
struct AnyExpr {};
struct EofExpr {};
struct FailExpr {};
struct RefExpr { RuleId rule_id; };
struct SeqExpr { std::vector<PegExprPtr> items; };
struct ChoiceExpr { std::vector<PegExprPtr> alts; };
struct RepeatExpr {
    PegExprPtr body;
    size_t min_count = 0;
    size_t max_count = std::numeric_limits<size_t>::max();
};
struct LookaheadExpr {
    PegExprPtr body;
    bool positive = true; // true => &e, false => !e
};
struct FilterExpr {
    PegExprPtr body;
    std::function<bool(std::string_view, const std::vector<SyntaxNodePtr>&)> pred;
};

struct PegExpr {
    using Variant = std::variant<
        LitExpr,
        RangeExpr,
        CharSetExpr,
        AnyExpr,
        EofExpr,
        FailExpr,
        RefExpr,
        SeqExpr,
        ChoiceExpr,
        RepeatExpr,
        LookaheadExpr,
        FilterExpr>;

    Variant node;

    template <typename T>
    explicit PegExpr(T&& v) : node(std::forward<T>(v)) {}
};

// Builder helpers
PegExprPtr lit(std::string text);
PegExprPtr ch(char c);
PegExprPtr range(uint8_t lo, uint8_t hi);
PegExprPtr char_set(std::string chars);
PegExprPtr any_byte();
PegExprPtr eof();
PegExprPtr fail_expr();
PegExprPtr seq(std::vector<PegExprPtr> items);
PegExprPtr choice(std::vector<PegExprPtr> alts);
PegExprPtr opt(PegExprPtr body);
PegExprPtr zero_or_more(PegExprPtr body);
PegExprPtr one_or_more(PegExprPtr body);
PegExprPtr and_pred(PegExprPtr body);
PegExprPtr not_pred(PegExprPtr body);
PegExprPtr filter(
    PegExprPtr body,
    std::function<bool(std::string_view, const std::vector<SyntaxNodePtr>&)> pred);

// -----------------------------------------------------------------------------
// Runtime-Extensible Grammar Table
// -----------------------------------------------------------------------------
struct Rule {
    RuleId id = 0;
    std::string name;
    PegExprPtr expr;
    bool hidden = false;    // hidden rules do not emit SyntaxNodes into parent's children
    bool cacheable = true;  // memoize in packrat table
};

class Grammar {
private:
    std::vector<Rule> rules_;
    std::unordered_map<std::string, RuleId> name_to_id_;
    uint32_t fresh_counter_ = 0;

public:
    RuleId ensure_rule(std::string_view name);
    RuleId define(std::string_view name, PegExprPtr expr, bool hidden = false, bool cacheable = true);
    PegExprPtr get_expr(std::string_view name) const;
    RuleId fresh_rule(std::string_view prefix, PegExprPtr expr, bool hidden = false, bool cacheable = true);
    PegExprPtr ref(std::string_view name);
    PegExprPtr ref(RuleId id) const;

    void prepend_choice(std::string_view name, PegExprPtr alt);
    void append_choice(std::string_view name, PegExprPtr alt);

    template <typename Builder>
    RuleId wrap_rule(std::string_view target_name, std::string_view layer_name, Builder&& builder) {
        RuleId target_id = ensure_rule(target_name);
        PegExprPtr prev_expr = rules_[target_id].expr;
        if (!prev_expr) {
            throw std::runtime_error(
                "Peg::Grammar::wrap_rule: target rule '" + std::string(target_name) + "' is undefined");
        }

        PegExprPtr inner_ref;
        if (std::holds_alternative<RefExpr>(prev_expr->node)) {
            inner_ref = prev_expr;
        } else {
            RuleId snapshot_id = fresh_rule(
                target_name, std::move(prev_expr), rules_[target_id].hidden, rules_[target_id].cacheable);
            inner_ref = ref(snapshot_id);
        }

        RuleId layer_id = define(layer_name, builder(inner_ref));
        rules_[target_id].expr = ref(layer_id);
        return layer_id;
    }

    const Rule& rule(RuleId id) const { return rules_.at(id); }
    std::optional<RuleId> find_rule(std::string_view name) const;
    size_t rule_count() const noexcept { return rules_.size(); }
};

// -----------------------------------------------------------------------------
// Strict O(N * R) Packrat Parser (No Left-Recursion; Fast Pos-Indexed Memo)
// -----------------------------------------------------------------------------
class PackratParser {
private:
    struct ParseResult {
        bool ok = false;
        size_t next_pos = 0;
        std::vector<SyntaxNodePtr> nodes;
    };

    struct MemoEntry {
        ParseResult result;
        bool in_progress = false;
    };

    const Grammar& grammar_;
    std::string_view input_;
    std::vector<std::unordered_map<RuleId, MemoEntry>> memo_by_pos_;
    size_t furthest_pos_ = 0;
    std::vector<std::string> expected_hints_;
    int lookahead_depth_ = 0;

    void record_failure(size_t pos, std::string_view hint);
    ParseResult wrap_rule_result(RuleId rule_id, size_t start_pos, ParseResult raw) const;
    ParseResult eval_rule(RuleId rule_id, size_t pos);
    ParseResult eval_expr(const PegExpr& expr, size_t pos);

public:
    PackratParser(const Grammar& grammar, std::string_view input);
    SyntaxNodePtr parse(std::string_view start_rule);
};

// -----------------------------------------------------------------------------
// Decoupled Semantic Evaluator (O(1) RuleId-indexed dispatch, CST -> R)
// -----------------------------------------------------------------------------
template <typename R, typename... Args>
class Evaluator {
public:
    struct Context {
        const Evaluator& eval;
        const Grammar& grammar;
        std::string_view input;
        const SyntaxNode& node;

        size_t size() const noexcept { return node.children.size(); }
        std::string_view view() const noexcept { return node.view(input); }
        RuleId rule_id() const noexcept { return node.rule_id; }
        std::string_view rule_name() const { return grammar.rule(node.rule_id).name; }
        SourceLocation location() const noexcept { return compute_location(input, node.begin); }

        const SyntaxNode& child_node(size_t idx) const { return *node.children.at(idx); }
        RuleId child_rule_id(size_t idx) const { return node.children.at(idx)->rule_id; }
        std::string_view child_rule_name(size_t idx) const {
            return grammar.rule(node.children.at(idx)->rule_id).name;
        }
        std::string_view child_view(size_t idx) const {
            return node.children.at(idx)->view(input);
        }

        R evaluate_child(size_t idx, Args... args) const {
            return eval.evaluate(grammar, input, *node.children.at(idx), std::forward<Args>(args)...);
        }
    };

    using Callback = std::function<R(const Context&, Args...)>;

private:
    std::vector<Callback> handlers_;

public:
    void on(RuleId id, Callback cb) {
        if (id >= handlers_.size()) {
            handlers_.resize(static_cast<size_t>(id) + 1);
        }
        handlers_[id] = std::move(cb);
    }

    void on(Grammar& grammar, std::string_view rule_name, Callback cb) {
        on(grammar.ensure_rule(rule_name), std::move(cb));
    }

    R evaluate(const Grammar& grammar, std::string_view input, const SyntaxNode& node, Args... args) const {
        if (node.rule_id < handlers_.size() && handlers_[node.rule_id]) {
            Context ctx{*this, grammar, input, node};
            return handlers_[node.rule_id](ctx, std::forward<Args>(args)...);
        }
        if (node.children.size() == 1) {
            return evaluate(grammar, input, *node.children[0], std::forward<Args>(args)...);
        }
        throw ParseError(
            compute_location(input, node.begin),
            "no semantic handler registered for rule '" + grammar.rule(node.rule_id).name + "'");
    }
};

} // namespace reverSilly::Lang::Peg
