#include "../ast/AstCodec.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace reverSilly::Lang::Ast;

namespace {

bool eq_shape(const AstShape& a, const AstShape& b);
bool eq_value(const AstValue& a, const AstValue& b);
bool eq_expr(const ExprPtr& a, const ExprPtr& b);

bool eq_shape(const AstShape& a, const AstShape& b) {
    if (a.size != b.size || a.align != b.align || a.fields.size() != b.fields.size()) return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        if (a.fields[i].name != b.fields[i].name) return false;
        if (a.fields[i].offset != b.fields[i].offset) return false;
        if (a.fields[i].mutability != b.fields[i].mutability) return false;
        if (!eq_shape(a.fields[i].shape, b.fields[i].shape)) return false;
    }
    return true;
}

bool eq_intrinsic(const AstIntrinsicSpec& a, const AstIntrinsicSpec& b) {
    if (a.index() != b.index()) return false;
    if (auto* x = std::get_if<AstIntBinOp>(&a)) {
        const auto& y = std::get<AstIntBinOp>(b);
        return x->op == y.op && x->width == y.width && x->lhs == y.lhs && x->rhs == y.rhs;
    }
    if (auto* x = std::get_if<AstIntUnaryOp>(&a)) {
        const auto& y = std::get<AstIntUnaryOp>(b);
        return x->op == y.op && x->width == y.width && x->src == y.src;
    }
    if (auto* x = std::get_if<AstFloatBinOp>(&a)) {
        const auto& y = std::get<AstFloatBinOp>(b);
        return x->op == y.op && x->width == y.width && x->lhs == y.lhs && x->rhs == y.rhs;
    }
    if (auto* x = std::get_if<AstCastOp>(&a)) {
        const auto& y = std::get<AstCastOp>(b);
        return x->op == y.op && x->from_width == y.from_width && x->to_width == y.to_width && x->src == y.src;
    }
    if (auto* x = std::get_if<AstCmpOp>(&a)) {
        const auto& y = std::get<AstCmpOp>(b);
        return x->op == y.op && x->width == y.width && x->lhs == y.lhs && x->rhs == y.rhs;
    }
    if (auto* x = std::get_if<AstRuntimeOp>(&a)) {
        return x->op == std::get<AstRuntimeOp>(b).op;
    }
    return false;
}

bool eq_value(const AstValue& a, const AstValue& b) {
    if (a.data.index() != b.data.index()) return false;
    if (auto* x = std::get_if<RawBytes>(&a.data)) {
        const auto& y = std::get<RawBytes>(b.data);
        return x->align == y.align && x->bytes == y.bytes;
    }
    if (auto* x = std::get_if<SymbolVal>(&a.data)) {
        return x->name == std::get<SymbolVal>(b.data).name;
    }
    if (auto* x = std::get_if<ShapeVal>(&a.data)) {
        return eq_shape(x->shape, std::get<ShapeVal>(b.data).shape);
    }
    if (auto* x = std::get_if<AstProtoVal>(&a.data)) {
        return eq_expr(x->ast, std::get<AstProtoVal>(b.data).ast);
    }
    if (auto* x = std::get_if<IntrinsicVal>(&a.data)) {
        return eq_intrinsic(x->spec, std::get<IntrinsicVal>(b.data).spec);
    }
    if (auto* x = std::get_if<FfiExternVal>(&a.data)) {
        const auto& y = std::get<FfiExternVal>(b.data);
        return x->lib_name == y.lib_name && x->symbol_name == y.symbol_name
            && eq_shape(x->args, y.args) && eq_shape(x->ret, y.ret) && x->cc == y.cc;
    }
    if (auto* x = std::get_if<FfiBindPtrVal>(&a.data)) {
        const auto& y = std::get<FfiBindPtrVal>(b.data);
        return x->fn_slot == y.fn_slot && eq_shape(x->args, y.args) && eq_shape(x->ret, y.ret) && x->cc == y.cc;
    }
    if (auto* x = std::get_if<StructVal>(&a.data)) {
        const auto& y = std::get<StructVal>(b.data);
        if (x->size != y.size || x->align != y.align || x->fields.size() != y.fields.size()) return false;
        for (size_t i = 0; i < x->fields.size(); ++i) {
            if (x->fields[i].name != y.fields[i].name) return false;
            if (x->fields[i].offset != y.fields[i].offset) return false;
            if (x->fields[i].mutability != y.fields[i].mutability) return false;
            if (!eq_value(x->fields[i].value, y.fields[i].value)) return false;
        }
        return true;
    }
    return false;
}

bool eq_expr(const ExprPtr& a, const ExprPtr& b) {
    if (!a || !b) return !a && !b;
    if (a->node.index() != b->node.index()) return false;
    if (auto* x = std::get_if<WordExpr>(&a->node)) {
        return x->text == std::get<WordExpr>(b->node).text;
    }
    if (auto* x = std::get_if<BuiltinExpr>(&a->node)) {
        const auto& y = std::get<BuiltinExpr>(b->node);
        if (x->kind != y.kind || x->args.size() != y.args.size()) return false;
        for (size_t i = 0; i < x->args.size(); ++i) {
            if (!eq_expr(x->args[i], y->args[i])) return false;
        }
        return true;
    }
    if (auto* x = std::get_if<SequenceExpr>(&a->node)) {
        const auto& y = std::get<SequenceExpr>(b->node);
        if (x->items.size() != y.items.size()) return false;
        for (size_t i = 0; i < x->items.size(); ++i) {
            if (!eq_expr(x->items[i], y->items[i])) return false;
        }
        return true;
    }
    if (auto* x = std::get_if<ValueExpr>(&a->node)) {
        return eq_value(x->value, std::get<ValueExpr>(b->node).value);
    }
    return false;
}

template <typename Fn>
void expect_throws(Fn&& fn, const char* label) {
    bool threw = false;
    try {
        fn();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "FAIL: expected std::runtime_error for " << label << "\n";
        std::abort();
    }
}

void test_all_variants_roundtrip() {
    AstShape raw8{8, 8, {}};
    AstShape pair_shape{
        16, 8,
        {
            AstField{"a", 0, raw8, 0},
            AstField{"b", 8, raw8, 1},
        }};

    ExprPtr inner_body = make_expr<BuiltinExpr>(
        BuiltinKind::Break,
        std::vector<ExprPtr>{make_expr<WordExpr>("inner_result")});

    StructVal nested_struct{
        16, 8,
        {
            AstStructField{"raw", 0, AstValue{RawBytes{4, {0xDE, 0xAD, 0xBE, 0xEF}}}, 1},
            AstStructField{"sym", 8, AstValue{SymbolVal{"hello_world"}}, 0},
        }};

    std::vector<ExprPtr> items;
    items.push_back(make_expr<WordExpr>("alpha"));
    items.push_back(make_expr<BuiltinExpr>(BuiltinKind::This, std::vector<ExprPtr>{}));
    items.push_back(make_expr<ValueExpr>(AstValue{RawBytes{1, {1, 2, 3, 0, 255}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{SymbolVal{"my_sym"}}));
    items.push_back(make_expr<ValueExpr>(AstValue{ShapeVal{pair_shape}}));
    items.push_back(make_expr<ValueExpr>(AstValue{AstProtoVal{inner_body}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstIntBinOp{IntBinKind::SDiv, 8, "lhs", "rhs"}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstIntUnaryOp{IntUnaryKind::BitNot, 4, "src"}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstFloatBinOp{FloatBinKind::Mul, 8, "x", "y"}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstCastOp{CastKind::SIToFP, 4, 8, "v"}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstCmpOp{CmpKind::SLt, 8, "l", "r"}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstRuntimeOp{RuntimeIntrinsicKind::InternShape}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstRuntimeOp{RuntimeIntrinsicKind::FieldMapCount}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{IntrinsicVal{AstRuntimeOp{RuntimeIntrinsicKind::BytesConcat}}}));
    items.push_back(make_expr<ValueExpr>(AstValue{FfiExternVal{"libc.so.6", "puts", pair_shape, raw8, "c"}}));
    items.push_back(make_expr<ValueExpr>(AstValue{FfiBindPtrVal{"fn_ptr", pair_shape, raw8, "c"}}));
    items.push_back(make_expr<ValueExpr>(AstValue{nested_struct}));

    ExprPtr root = make_expr<SequenceExpr>(std::move(items));

    std::vector<uint8_t> bytes = serialize(root);
    assert(bytes.size() > 12);

    ExprPtr decoded = deserialize(bytes);
    assert(eq_expr(root, decoded));
}

void test_dag_sharing_preserved() {
    ExprPtr shared_leaf = make_expr<BuiltinExpr>(
        BuiltinKind::Field,
        std::vector<ExprPtr>{
            make_expr<BuiltinExpr>(BuiltinKind::This, std::vector<ExprPtr>{}),
            make_expr<WordExpr>("shared_slot"),
        });

    ExprPtr proto_holder = make_expr<ValueExpr>(AstValue{AstProtoVal{shared_leaf}});

    ExprPtr root = make_expr<SequenceExpr>(std::vector<ExprPtr>{
        shared_leaf,
        make_expr<BuiltinExpr>(BuiltinKind::Deref, std::vector<ExprPtr>{shared_leaf}),
        proto_holder,
    });

    std::vector<uint8_t> bytes = serialize(root);
    ExprPtr decoded = deserialize(bytes);
    assert(eq_expr(root, decoded));

    const auto& seq = std::get<SequenceExpr>(decoded->node);
    assert(seq.items.size() == 3);
    const ExprPtr& ref0 = seq.items[0];
    const ExprPtr& ref1 = std::get<BuiltinExpr>(seq.items[1]->node).args[0];
    const ExprPtr& ref2 = std::get<AstProtoVal>(std::get<ValueExpr>(seq.items[2]->node).value.data).ast;

    assert(ref0.get() == ref1.get());
    assert(ref0.get() == ref2.get());
}

void test_error_cases() {
    expect_throws([] { (void)serialize(ExprPtr{}); }, "null root serialize");

    std::vector<uint8_t> empty;
    expect_throws([&] { (void)deserialize(empty); }, "empty bytes");

    ExprPtr simple = make_expr<WordExpr>("ok");
    std::vector<uint8_t> valid = serialize(simple);

    // Bad magic
    std::vector<uint8_t> bad_magic = valid;
    bad_magic[0] = 'X';
    expect_throws([&] { (void)deserialize(bad_magic); }, "bad magic");

    // Wrong version
    std::vector<uint8_t> bad_ver = valid;
    bad_ver[8] = 99;
    expect_throws([&] { (void)deserialize(bad_ver); }, "wrong version");

    // Truncated payload
    std::vector<uint8_t> truncated(valid.begin(), valid.begin() + 14);
    expect_throws([&] { (void)deserialize(truncated); }, "truncated payload");

    // Trailing garbage
    std::vector<uint8_t> trailing = valid;
    trailing.push_back(0x42);
    expect_throws([&] { (void)deserialize(trailing); }, "trailing bytes");
}

} // namespace

int main() {
    test_all_variants_roundtrip();
    test_dag_sharing_preserved();
    test_error_cases();
    std::cout << "test_ast_codec: all tests passed.\n";
    return 0;
}
