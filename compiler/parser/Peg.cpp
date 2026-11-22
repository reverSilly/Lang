#include "Peg.h"

namespace reverSilly::Lang::Peg {

SourceLocation compute_location(std::string_view input, size_t offset) noexcept {
    SourceLocation loc{offset, 1, 1};
    size_t limit = offset < input.size() ? offset : input.size();
    for (size_t i = 0; i < limit; ++i) {
        if (input[i] == '\n') {
            ++loc.line;
            loc.col = 1;
        } else {
            ++loc.col;
        }
    }
    return loc;
}

PegExprPtr lit(std::string text) { return std::make_shared<PegExpr>(LitExpr{std::move(text)}); }
PegExprPtr ch(char c) { return std::make_shared<PegExpr>(LitExpr{std::string(1, c)}); }
PegExprPtr range(uint8_t lo, uint8_t hi) { return std::make_shared<PegExpr>(RangeExpr{lo, hi}); }
PegExprPtr char_set(std::string chars) { return std::make_shared<PegExpr>(CharSetExpr{std::move(chars)}); }
PegExprPtr any_byte() { return std::make_shared<PegExpr>(AnyExpr{}); }
PegExprPtr eof() { return std::make_shared<PegExpr>(EofExpr{}); }
PegExprPtr fail_expr() { return std::make_shared<PegExpr>(FailExpr{}); }
PegExprPtr seq(std::vector<PegExprPtr> items) { return std::make_shared<PegExpr>(SeqExpr{std::move(items)}); }
PegExprPtr choice(std::vector<PegExprPtr> alts) { return std::make_shared<PegExpr>(ChoiceExpr{std::move(alts)}); }
PegExprPtr opt(PegExprPtr body) { return std::make_shared<PegExpr>(RepeatExpr{std::move(body), 0, 1}); }
PegExprPtr zero_or_more(PegExprPtr body) {
    return std::make_shared<PegExpr>(RepeatExpr{std::move(body), 0, std::numeric_limits<size_t>::max()});
}
PegExprPtr one_or_more(PegExprPtr body) {
    return std::make_shared<PegExpr>(RepeatExpr{std::move(body), 1, std::numeric_limits<size_t>::max()});
}
PegExprPtr and_pred(PegExprPtr body) { return std::make_shared<PegExpr>(LookaheadExpr{std::move(body), true}); }
PegExprPtr not_pred(PegExprPtr body) { return std::make_shared<PegExpr>(LookaheadExpr{std::move(body), false}); }
PegExprPtr filter(
    PegExprPtr body,
    std::function<bool(std::string_view, const std::vector<SyntaxNodePtr>&)> pred) {
    return std::make_shared<PegExpr>(FilterExpr{std::move(body), std::move(pred)});
}

RuleId Grammar::ensure_rule(std::string_view name) {
    std::string key(name);
    auto it = name_to_id_.find(key);
    if (it != name_to_id_.end()) {
        return it->second;
    }
    RuleId id = static_cast<RuleId>(rules_.size());
    rules_.push_back(Rule{id, key, nullptr, false, true});
    name_to_id_.emplace(std::move(key), id);
    return id;
}

RuleId Grammar::define(std::string_view name, PegExprPtr expr, bool hidden, bool cacheable) {
    RuleId id = ensure_rule(name);
    rules_[id].expr = std::move(expr);
    rules_[id].hidden = hidden;
    rules_[id].cacheable = cacheable;
    return id;
}

PegExprPtr Grammar::get_expr(std::string_view name) const {
    auto id_opt = find_rule(name);
    if (!id_opt) return nullptr;
    return rules_[*id_opt].expr;
}

RuleId Grammar::fresh_rule(std::string_view prefix, PegExprPtr expr, bool hidden, bool cacheable) {
    std::string unique_name = std::string(prefix) + "#" + std::to_string(fresh_counter_++);
    return define(unique_name, std::move(expr), hidden, cacheable);
}

PegExprPtr Grammar::ref(std::string_view name) {
    return std::make_shared<PegExpr>(RefExpr{ensure_rule(name)});
}

PegExprPtr Grammar::ref(RuleId id) const {
    return std::make_shared<PegExpr>(RefExpr{id});
}

void Grammar::prepend_choice(std::string_view name, PegExprPtr alt) {
    RuleId id = ensure_rule(name);
    Rule& r = rules_[id];
    if (!r.expr || std::holds_alternative<FailExpr>(r.expr->node)) {
        r.expr = std::move(alt);
        return;
    }
    if (auto* ch_expr = std::get_if<ChoiceExpr>(&r.expr->node)) {
        std::vector<PegExprPtr> next;
        next.reserve(ch_expr->alts.size() + 1);
        next.push_back(std::move(alt));
        next.insert(next.end(), ch_expr->alts.begin(), ch_expr->alts.end());
        r.expr = choice(std::move(next));
    } else {
        r.expr = choice({std::move(alt), r.expr});
    }
}

void Grammar::append_choice(std::string_view name, PegExprPtr alt) {
    RuleId id = ensure_rule(name);
    Rule& r = rules_[id];
    if (!r.expr || std::holds_alternative<FailExpr>(r.expr->node)) {
        r.expr = std::move(alt);
        return;
    }
    if (auto* ch_expr = std::get_if<ChoiceExpr>(&r.expr->node)) {
        std::vector<PegExprPtr> next = ch_expr->alts;
        next.push_back(std::move(alt));
        r.expr = choice(std::move(next));
    } else {
        r.expr = choice({r.expr, std::move(alt)});
    }
}

std::optional<RuleId> Grammar::find_rule(std::string_view name) const {
    auto it = name_to_id_.find(std::string(name));
    if (it == name_to_id_.end()) return std::nullopt;
    return it->second;
}

PackratParser::PackratParser(const Grammar& grammar, std::string_view input)
    : grammar_(grammar), input_(input), memo_by_pos_(input.size() + 1) {}

void PackratParser::record_failure(size_t pos, std::string_view hint) {
    if (lookahead_depth_ > 0) return;
    if (pos > furthest_pos_) {
        furthest_pos_ = pos;
        expected_hint_ = std::string(hint);
    } else if (pos == furthest_pos_ && expected_hint_.empty() && !hint.empty()) {
        expected_hint_ = std::string(hint);
    }
}

PackratParser::ParseResult PackratParser::wrap_rule_result(RuleId rule_id, size_t start_pos, ParseResult raw) const {
    if (!raw.ok) return raw;
    const Rule& r = grammar_.rule(rule_id);
    if (r.hidden) {
        raw.nodes.clear();
        return raw;
    }
    auto node = std::make_shared<SyntaxNode>();
    node->rule_id = rule_id;
    node->begin = start_pos;
    node->end = raw.next_pos;
    node->children = std::move(raw.nodes);
    ParseResult wrapped;
    wrapped.ok = true;
    wrapped.next_pos = node->end;
    wrapped.nodes.push_back(std::move(node));
    return wrapped;
}

PackratParser::ParseResult PackratParser::eval_rule(RuleId rule_id, size_t pos) {
    const Rule& r = grammar_.rule(rule_id);
    if (!r.expr) {
        throw std::runtime_error("Peg::PackratParser: undefined rule '" + r.name + "'");
    }
    if (!r.cacheable) {
        ParseResult res = eval_expr(*r.expr, pos);
        if (!res.ok) record_failure(pos, r.name);
        return wrap_rule_result(rule_id, pos, std::move(res));
    }

    auto& bucket = memo_by_pos_[pos];
    if (auto it = bucket.find(rule_id); it != bucket.end()) {
        if (it->second.in_progress) {
            throw ParseError(
                compute_location(input_, pos),
                "left-recursive grammar rule '" + r.name + "' is not allowed (use iteration or wrap_rule)");
        }
        return it->second.result;
    }

    bucket.emplace(rule_id, MemoEntry{ParseResult{false, pos, {}}, true});
    ParseResult raw = eval_expr(*r.expr, pos);
    if (!raw.ok) record_failure(pos, r.name);

    ParseResult final_res = wrap_rule_result(rule_id, pos, std::move(raw));
    auto& entry = memo_by_pos_[pos].at(rule_id);
    entry.result = final_res;
    entry.in_progress = false;
    return final_res;
}

PackratParser::ParseResult PackratParser::eval_expr(const PegExpr& expr, size_t pos) {
    return std::visit(
        [&](const auto& e) -> ParseResult {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, LitExpr>) {
                if (input_.substr(pos, e.text.size()) == e.text) {
                    return ParseResult{true, pos + e.text.size(), {}};
                }
                record_failure(pos, "'" + e.text + "'");
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, RangeExpr>) {
                if (pos < input_.size()) {
                    uint8_t b = static_cast<uint8_t>(input_[pos]);
                    if (b >= e.lo && b <= e.hi) {
                        return ParseResult{true, pos + 1, {}};
                    }
                }
                record_failure(pos, "byte range");
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, CharSetExpr>) {
                if (pos < input_.size() && e.chars.find(input_[pos]) != std::string::npos) {
                    return ParseResult{true, pos + 1, {}};
                }
                record_failure(pos, "character set");
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, AnyExpr>) {
                if (pos < input_.size()) {
                    return ParseResult{true, pos + 1, {}};
                }
                record_failure(pos, "any character");
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, EofExpr>) {
                if (pos == input_.size()) {
                    return ParseResult{true, pos, {}};
                }
                record_failure(pos, "end of input");
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, FailExpr>) {
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, RefExpr>) {
                return eval_rule(e.rule_id, pos);
            } else if constexpr (std::is_same_v<T, SeqExpr>) {
                size_t cur = pos;
                std::vector<SyntaxNodePtr> out;
                for (const auto& item : e.items) {
                    ParseResult r = eval_expr(*item, cur);
                    if (!r.ok) return ParseResult{false, pos, {}};
                    cur = r.next_pos;
                    for (auto& n : r.nodes) out.push_back(std::move(n));
                }
                return ParseResult{true, cur, std::move(out)};
            } else if constexpr (std::is_same_v<T, ChoiceExpr>) {
                for (const auto& alt : e.alts) {
                    ParseResult r = eval_expr(*alt, pos);
                    if (r.ok) return r;
                }
                return ParseResult{false, pos, {}};
            } else if constexpr (std::is_same_v<T, RepeatExpr>) {
                size_t cur = pos;
                size_t count = 0;
                std::vector<SyntaxNodePtr> out;
                while (count < e.max_count) {
                    ParseResult r = eval_expr(*e.body, cur);
                    if (!r.ok) break;
                    if (r.next_pos == cur) {
                        ++count;
                        for (auto& n : r.nodes) out.push_back(std::move(n));
                        break;
                    }
                    cur = r.next_pos;
                    ++count;
                    for (auto& n : r.nodes) out.push_back(std::move(n));
                }
                if (count < e.min_count) {
                    return ParseResult{false, pos, {}};
                }
                return ParseResult{true, cur, std::move(out)};
            } else if constexpr (std::is_same_v<T, LookaheadExpr>) {
                ++lookahead_depth_;
                ParseResult r = eval_expr(*e.body, pos);
                --lookahead_depth_;
                bool ok = e.positive ? r.ok : !r.ok;
                return ParseResult{ok, pos, {}};
            } else if constexpr (std::is_same_v<T, FilterExpr>) {
                ParseResult r = eval_expr(*e.body, pos);
                if (!r.ok) return r;
                if (!e.pred(input_.substr(pos, r.next_pos - pos), r.nodes)) {
                    return ParseResult{false, pos, {}};
                }
                return r;
            }
        },
        expr.node);
}

SyntaxNodePtr PackratParser::parse(std::string_view start_rule) {
    auto id_opt = grammar_.find_rule(start_rule);
    if (!id_opt) {
        throw std::runtime_error("Peg::PackratParser: unknown start rule '" + std::string(start_rule) + "'");
    }
    ParseResult res = eval_rule(*id_opt, 0);
    if (!res.ok || res.nodes.empty()) {
        SourceLocation loc = compute_location(input_, furthest_pos_);
        std::string msg = expected_hint_.empty()
            ? "unexpected syntax"
            : ("expected " + expected_hint_);
        throw ParseError(loc, msg);
    }
    return res.nodes.front();
}

} // namespace reverSilly::Lang::Peg
