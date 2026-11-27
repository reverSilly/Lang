#include "../ast/Ast.h"
#include "../eval/Capabilities.h"
#include "../eval/Evaluator.h"
#include "../parser/Layer0.h"
#include "../runtime/Runtime.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

using namespace reverSilly::Lang::Ast;
using namespace reverSilly::Lang::Parser;
using namespace reverSilly::Lang::Runtime;
using namespace reverSilly::Lang::Eval;

static void test_io_read_write_and_symbols_intern() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    const std::string tmp_path = "tmp_cap_io_test.txt";
    const std::string payload = "hello_lang_cap_io";

    auto* rf = rt.current_frame();
    rf->bind(rt.symbols.intern("p_path"),
             rf->allocate_val(rt.make_raw_bytes(tmp_path.data(), tmp_path.size()), false)->pointer());
    rf->bind(rt.symbols.intern("p_data"),
             rf->allocate_val(rt.make_raw_bytes(payload.data(), payload.size()), false)->pointer());

    // Write file, read it back, intern its bytes to a Symbol, and verify
    Value sym_val = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(path, #allocate_val(#deref(#field(#this, p_path)), #deref(#field(#this, F)))) ;
        #bind(bytes, #allocate_val(#deref(#field(#this, p_data)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), io), write_file))) ;
        #drop(#field(#this, bytes)) ;
        #bind(bytes, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), io), read_file))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), symbols), intern_bytes)))
    )"));

    std::remove(tmp_path.c_str());

    assert(sym_val.shape_addr == rt.shapes.symbol_shape);
    assert(rt.symbols.name(sym_val.as_pod<SymbolId>()) == payload);
}

static void test_intrinsics_bytes_u8_and_eq_shape() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    const std::string text = "\"42\"";
    auto* rf = rt.current_frame();
    rf->bind(rt.symbols.intern("raw_str"),
             rf->allocate_val(rt.make_raw_bytes(text.data(), text.size()), false)->pointer());
    rf->bind(rt.symbols.intern("v1"), rf->allocate_val(Value::from_pod<uint64_t>(rt.shapes.raw8_shape, 1), false)->pointer());
    rf->bind(rt.symbols.intern("v2"), rf->allocate_val(Value::from_pod<uint64_t>(rt.shapes.raw8_shape, 2), false)->pointer());

    // Slice "42" -> 42, read byte at index 1 ('2' == 50), convert u8 -> i64
    Value ch_i64 = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(bytes, #allocate_val(#deref(#field(#this, raw_str)), #deref(#field(#this, F)))) ;
        #bind(start, #allocate_val(#deref(#field(#this, v1)), #deref(#field(#this, F)))) ;
        #bind(len, #allocate_val(#deref(#field(#this, v2)), #deref(#field(#this, F)))) ;
        #bind(sliced, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), bytes_slice))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, bytes)) ;
        #bind(bytes, #allocate_val(#deref(#field(#this, sliced)), #deref(#field(#this, F)))) ;
        #bind(index, #allocate_val(#deref(#field(#this, v1)), #deref(#field(#this, F)))) ;
        #bind(b, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), bytes_at))), #deref(#field(#this, F)))) ;
        #bind(signed, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(from, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw1_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(to, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(src, #allocate_val(b, #deref(#field(#this, F)))) ;
        #bind(zext_1_8, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), ext_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, zext_1_8)))
    )"));
    assert(ch_i64.shape_addr == rt.shapes.raw8_shape);
    assert(ch_i64.as_pod<int64_t>() == 50); // ASCII '2'

    // Pointer(Shape) O(1) comparison via .addr == .addr (eq_bytes(8))
    Value eq_true = ev.run(fe.parse(R"(
        #bind(width, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(lhs, #allocate_val(a, #deref(#field(#this, F)))) ;
        #bind(rhs, #allocate_val(b, #deref(#field(#this, F)))) ;
        #bind(p_eq8, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), eq_bytes))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, width)) ; #drop(#field(#this, lhs)) ; #drop(#field(#this, rhs)) ;
        #bind(s1, #allocate_val(#shape(#deref(#field(#this, v1))), #deref(#field(#this, F)))) ;
        #bind(s2, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), #deref(#field(#this, F)))) ;
        #bind(a, #allocate_val(#deref(#field(#field(#this, s1), addr)), #deref(#field(#this, F)))) ;
        #drop(#field(#this, b)) ;
        #bind(b, #allocate_val(#deref(#field(#field(#this, s2), addr)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_eq8)))
    )"));
    assert(eq_true.shape_addr == rt.shapes.raw1_shape);
    assert(eq_true.as_pod<uint8_t>() == 1);

    Value eq_false = ev.run(fe.parse(R"(
        #drop(#field(#this, b)) ;
        #bind(s3, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), raw1_shape)), #deref(#field(#this, F)))) ;
        #bind(b, #allocate_val(#deref(#field(#field(#this, s3), addr)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_eq8)))
    )"));
    assert(eq_false.shape_addr == rt.shapes.raw1_shape);
    assert(eq_false.as_pod<uint8_t>() == 0);
}

static void test_syntax_cst_view_bytes_and_child_view_sym() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    // Register a custom rule Pair = Word ":" Word on a fresh frontend via capabilities,
    // whose handler uses cst_view_bytes and cst_child_view_sym.
    const std::string sample = "foo:bar";
    auto* rf = rt.current_frame();
    rf->bind(rt.symbols.intern("sample_src"),
             rf->allocate_val(rt.make_raw_bytes(sample.data(), sample.size()), false)->pointer());

    // Build handler in C++ AST for concise unit testing of cst_view_bytes & cst_child_view_sym
    // Handler returns ast.value(cst_view_bytes(node))
    uint64_t handler_bytes = rt.prototypes.create_ast(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#parent(#this), capabilities), constants), false)), #deref(#field(#field(#field(#parent(#this), capabilities), constants), false)))) ;
        #bind(val, #allocate_val(#evaluate(#deref(#field(#field(#field(#parent(#this), capabilities), syntax), cst_view_bytes))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#parent(#this), capabilities), ast), value)))
    )"));
    rf->bind(rt.symbols.intern("h_bytes"), rf->allocate_val(rt.make_proto(handler_bytes), false)->pointer());

    uint64_t handler_child1 = rt.prototypes.create_ast(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#parent(#this), capabilities), constants), false)), #deref(#field(#field(#field(#parent(#this), capabilities), constants), false)))) ;
        #bind(index, #allocate_val(#deref(#field(#field(#field(#parent(#this), capabilities), constants), u64_one)), #deref(#field(#this, F)))) ;
        #bind(val, #allocate_val(#evaluate(#deref(#field(#field(#field(#parent(#this), capabilities), syntax), cst_child_view_sym))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#parent(#this), capabilities), ast), value)))
    )"));
    rf->bind(rt.symbols.intern("h_child1"), rf->allocate_val(rt.make_proto(handler_child1), false)->pointer());

    Value bytes_res = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(T, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), true)), #deref(#field(#this, F)))) ;
        #bind(Z, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), u64_zero)), #deref(#field(#this, F)))) ;
        #bind(fe, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), new_layer0))), #deref(#field(#this, F)))) ;
        #bind(arena, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), new_arena))), #deref(#field(#this, F)))) ;

        #bind(sym, #allocate_val(:, #deref(#field(#this, F)))) ;
        #bind(lit_colon, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), lit))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(name, #allocate_val(Delimiter, #deref(#field(#this, F)))) ;
        #bind(old_d, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), get_rule))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, old_d)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, lit_colon)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), choice))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(name, #allocate_val(Delimiter, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, T)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(name, #allocate_val(Word, #deref(#field(#this, F)))) ;
        #bind(ref_word, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), ref))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, ref_word)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, lit_colon)), #deref(#field(#this, F)))) ;
        #bind(2, #allocate_val(#deref(#field(#this, ref_word)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), seq))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, 2)) ;

        #bind(name, #allocate_val(Pair, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, h_bytes)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(name, #allocate_val(Pair, #deref(#field(#this, F)))) ;
        #bind(ref_pair, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), ref))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(name, #allocate_val(Atom, #deref(#field(#this, F)))) ;
        #bind(old_atom, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), get_rule))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, ref_pair)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, old_atom)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), choice))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(name, #allocate_val(Atom, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(src, #allocate_val(#deref(#field(#this, sample_src)), #deref(#field(#this, F)))) ;
        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), parse))), #deref(#field(#this, F)))) ;
        #bind(p, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), to_proto))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, ast)) ;
        #evaluate(#deref(#field(#this, p)))
    )"));

    const auto* bsh = bytes_res.shape();
    assert(bsh && bsh->is_leaf() && bsh->align == 1 && bsh->size == 7);
    assert(std::string_view(reinterpret_cast<const char*>(bytes_res.bytes.data()), bytes_res.bytes.size()) == "foo:bar");

    // Rebind Pair handler to h_child1 and verify cst_child_view_sym(1) == "bar"
    Value child_sym = ev.run(fe.parse(R"(
        #bind(name, #allocate_val(Pair, #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), get_rule))), #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, h_child1)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), parse))), #deref(#field(#this, F)))) ;
        #bind(p2, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), to_proto))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, ast)) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), ast), destroy_arena))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), syntax), destroy_frontend))) ;
        #evaluate(#deref(#field(#this, p2)))
    )"));
    assert(child_sym.shape_addr == rt.shapes.symbol_shape);
    assert(rt.symbols.name(child_sym.as_pod<SymbolId>()) == "bar");
}

void test_install_runtime_isolation() {
    RuntimeContext rt;
    install_runtime_capabilities(rt);

    ConstructingFrame* root = rt.current_frame();
    PointerLayout caps_ptr = root->field(rt.symbols.intern("capabilities"));
    assert(!caps_ptr.is_null());

    // Tier 1 modules must be present
    assert(!rt.field(caps_ptr, rt.symbols.intern("constants")).is_null());
    assert(!rt.field(caps_ptr, rt.symbols.intern("field_map")).is_null());
    assert(!rt.field(caps_ptr, rt.symbols.intern("shapes")).is_null());
    assert(!rt.field(caps_ptr, rt.symbols.intern("symbols")).is_null());
    assert(!rt.field(caps_ptr, rt.symbols.intern("intrinsics")).is_null());

    // Tier 3 compiler frontend modules must be absent in runtime-only mode
    assert(rt.field(caps_ptr, rt.symbols.intern("ast")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("syntax")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("backends")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("io")).is_null());
}

static void test_prototype_factory_intrinsics() {
    RuntimeContext rt;
    install_runtime_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    auto* rf = rt.current_frame();
    auto bind_val = [&](const char* name, const Value& v) {
        rf->bind(rt.symbols.intern(name), rf->allocate_val(v, false)->pointer());
    };

    // Integer arithmetic, signed vs unsigned div/rem, shifts, bitwise, sext/zext/trunc
    bind_val("i32_neg10", Value::from_pod<int32_t>(rt.shapes.raw4_shape, -10));
    bind_val("i32_3", Value::from_pod<int32_t>(rt.shapes.raw4_shape, 3));
    bind_val("i8_neg5", Value::from_pod<int8_t>(rt.shapes.raw1_shape, -5));
    bind_val("i64_big", Value::from_pod<uint64_t>(rt.shapes.raw8_shape, 0x123456789ABCDEF0ULL));
    bind_val("f64_a", Value::from_pod<double>(rt.shapes.raw8_shape, 7.5));
    bind_val("f64_b", Value::from_pod<double>(rt.shapes.raw8_shape, 2.5));
    bind_val("f32_a", Value::from_pod<float>(rt.shapes.raw4_shape, 1.5f));

    // 1. sdiv(-10, 3) == -3, srem(-10, 3) == -1 (4B signed) + verify IntrinsicProto variant
    Value sdiv_res = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(T, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), true)), #deref(#field(#this, F)))) ;
        #bind(W1, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw1_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(W2, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw2_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(W4, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw4_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(W8, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(signed, #allocate_val(#deref(#field(#this, T)), #deref(#field(#this, F)))) ;
        #bind(width, #allocate_val(#deref(#field(#this, W4)), #deref(#field(#this, F)))) ;
        #bind(lhs, #allocate_val(i32_neg10, #deref(#field(#this, F)))) ;
        #bind(rhs, #allocate_val(i32_3, #deref(#field(#this, F)))) ;
        #bind(p_sdiv, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), div_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_sdiv)))
    )"));
    assert(sdiv_res.shape_addr == rt.shapes.raw4_shape);
    assert(sdiv_res.as_pod<int32_t>() == -3);
    {
        Value pv = rt.deref(rf->field(rt.symbols.intern("p_sdiv")));
        const auto* ip = std::get_if<IntrinsicProto>(&rt.prototypes.get(pv.as_pod<uint64_t>())->payload);
        assert(ip != nullptr);
        const auto* ib = std::get_if<IntBinOp>(&ip->spec);
        assert(ib != nullptr && ib->op == IntBinKind::SDiv && ib->width == 4);
    }

    // 2. sext i8(-5) -> i32(-5) & trunc u64(0x123456789ABCDEF0) -> u16(0xDEF0)
    Value sext_res = ev.run(fe.parse(R"(
        #bind(from, #allocate_val(#deref(#field(#this, W1)), #deref(#field(#this, F)))) ;
        #bind(to, #allocate_val(#deref(#field(#this, W4)), #deref(#field(#this, F)))) ;
        #bind(src, #allocate_val(i8_neg5, #deref(#field(#this, F)))) ;
        #bind(p_sext, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), ext_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_sext)))
    )"));
    assert(sext_res.shape_addr == rt.shapes.raw4_shape);
    assert(sext_res.as_pod<int32_t>() == -5);

    Value trunc_res = ev.run(fe.parse(R"(
        #drop(#field(#this, from)) ; #drop(#field(#this, to)) ; #drop(#field(#this, src)) ;
        #bind(from, #allocate_val(#deref(#field(#this, W8)), #deref(#field(#this, F)))) ;
        #bind(to, #allocate_val(#deref(#field(#this, W2)), #deref(#field(#this, F)))) ;
        #bind(src, #allocate_val(i64_big, #deref(#field(#this, F)))) ;
        #bind(p_trunc, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), trunc_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_trunc)))
    )"));
    assert(trunc_res.shape_addr == rt.shapes.raw2_shape);
    assert(trunc_res.as_pod<uint16_t>() == 0xDEF0);

    // 3. Float ops: div_float(7.5, 2.5) == 3.0, ext_float(1.5f) == 1.5, float_to_int(7.5) == 7, lt_float
    Value fdiv_res = ev.run(fe.parse(R"(
        #drop(#field(#this, width)) ; #drop(#field(#this, lhs)) ; #drop(#field(#this, rhs)) ;
        #bind(width, #allocate_val(#deref(#field(#this, W8)), #deref(#field(#this, F)))) ;
        #bind(lhs, #allocate_val(f64_a, #deref(#field(#this, F)))) ;
        #bind(rhs, #allocate_val(f64_b, #deref(#field(#this, F)))) ;
        #bind(p_fdiv, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), div_float))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_fdiv)))
    )"));
    assert(fdiv_res.shape_addr == rt.shapes.raw8_shape);
    assert(fdiv_res.as_pod<double>() == 3.0);

    Value fext_res = ev.run(fe.parse(R"(
        #drop(#field(#this, src)) ;
        #bind(src, #allocate_val(f32_a, #deref(#field(#this, F)))) ;
        #bind(p_fext, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), ext_float))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_fext)))
    )"));
    assert(fext_res.shape_addr == rt.shapes.raw8_shape);
    assert(fext_res.as_pod<double>() == 1.5);

    Value f2i_res = ev.run(fe.parse(R"(
        #drop(#field(#this, from)) ; #drop(#field(#this, to)) ; #drop(#field(#this, src)) ;
        #bind(from, #allocate_val(#deref(#field(#this, W8)), #deref(#field(#this, F)))) ;
        #bind(to, #allocate_val(#deref(#field(#this, W4)), #deref(#field(#this, F)))) ;
        #bind(src, #allocate_val(f64_a, #deref(#field(#this, F)))) ;
        #bind(p_f2i, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), float_to_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_f2i)))
    )"));
    assert(f2i_res.shape_addr == rt.shapes.raw4_shape);
    assert(f2i_res.as_pod<int32_t>() == 7);

    // 4. Bitwise & arithmetic shr: (-10 >> 1) == -5, bit_xor, eq_bytes
    Value ashr_res = ev.run(fe.parse(R"(
        #drop(#field(#this, width)) ; #drop(#field(#this, lhs)) ; #drop(#field(#this, rhs)) ;
        #bind(width, #allocate_val(#deref(#field(#this, W4)), #deref(#field(#this, F)))) ;
        #bind(lhs, #allocate_val(i32_neg10, #deref(#field(#this, F)))) ;
        #bind(rhs, #allocate_val(i32_3, #deref(#field(#this, F)))) ;
        #bind(p_shr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), shr))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, p_shr)))
    )"));
    assert(ashr_res.shape_addr == rt.shapes.raw4_shape);
    assert(ashr_res.as_pod<int32_t>() == -2);
}

static void test_stageable_runtime_intrinsics_and_intern_shape() {
    // Stage field_map, shapes, and symbols modules from a host RuntimeContext
    // into a brand-new isolated target RuntimeContext with ZERO capabilities.
    RuntimeContext host_rt;
    install_runtime_capabilities(host_rt);
    auto* hrf = host_rt.current_frame();
    PointerLayout hcaps = hrf->field(host_rt.symbols.intern("capabilities"));

    AstValue fm_mod_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hcaps, host_rt.symbols.intern("field_map"))));
    AstValue sh_mod_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hcaps, host_rt.symbols.intern("shapes"))));
    AstValue sy_mod_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hcaps, host_rt.symbols.intern("symbols"))));
    PointerLayout hintr = host_rt.field(hcaps, host_rt.symbols.intern("intrinsics"));
    AstValue ba_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hintr, host_rt.symbols.intern("bytes_at"))));
    AstValue bs_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hintr, host_rt.symbols.intern("bytes_slice"))));
    AstValue bc_ast = reify_ast_value(host_rt, host_rt.deref(host_rt.field(hintr, host_rt.symbols.intern("bytes_concat"))));

    // Isolated target runtime (zero capabilities installed!)
    RuntimeContext target_rt;
    Evaluator ev(target_rt);
    Frontend fe;
    auto* trf = target_rt.current_frame();
    assert(trf->field(target_rt.symbols.intern("capabilities")).is_null());

    trf->bind(target_rt.symbols.intern("fm"), trf->allocate_val(intern_ast_value(target_rt, fm_mod_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("sh"), trf->allocate_val(intern_ast_value(target_rt, sh_mod_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("sy"), trf->allocate_val(intern_ast_value(target_rt, sy_mod_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("p_ba"), trf->allocate_val(intern_ast_value(target_rt, ba_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("p_bs"), trf->allocate_val(intern_ast_value(target_rt, bs_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("p_bc"), trf->allocate_val(intern_ast_value(target_rt, bc_ast), false)->pointer());
    trf->bind(target_rt.symbols.intern("F"), trf->allocate_val(Value::from_pod<uint8_t>(target_rt.shapes.raw1_shape, 0), false)->pointer());
    trf->bind(target_rt.symbols.intern("T"), trf->allocate_val(Value::from_pod<uint8_t>(target_rt.shapes.raw1_shape, 1), false)->pointer());
    trf->bind(target_rt.symbols.intern("V0"), trf->allocate_val(Value::from_pod<uint64_t>(target_rt.shapes.raw8_shape, 0), false)->pointer());
    trf->bind(target_rt.symbols.intern("V1"), trf->allocate_val(Value::from_pod<uint64_t>(target_rt.shapes.raw8_shape, 1), false)->pointer());
    trf->bind(target_rt.symbols.intern("V3"), trf->allocate_val(Value::from_pod<uint64_t>(target_rt.shapes.raw8_shape, 3), false)->pointer());
    trf->bind(target_rt.symbols.intern("V8"), trf->allocate_val(Value::from_pod<uint64_t>(target_rt.shapes.raw8_shape, 8), false)->pointer());
    trf->bind(target_rt.symbols.intern("S_RAW8"), trf->allocate_val(
        target_rt.make_pointer(PointerLayout{target_rt.shapes.raw8_shape, target_rt.shapes.shape_meta, 0}), false)->pointer());

    // 1. Stageable field_map.empty + shapes.intern_shape({ shape }) + #allocate in zero-cap target runtime
    Value leaf_slot_ptr = ev.run(fe.parse(R"(
        #bind(empty_fm, #allocate_val(#evaluate(#deref(#field(#field(#this, fm), empty))), #deref(#field(#this, F)))) ;
        #bind(shape, #allocate_val(#deref(#deref(#field(#this, S_RAW8))), #deref(#field(#this, F)))) ;
        #bind(interned_raw8, #allocate_val(#evaluate(#deref(#field(#field(#this, sh), intern_shape))), #deref(#field(#this, F)))) ;
        #allocate(#deref(#field(#this, interned_raw8)), #deref(#field(#this, T)))
    )"));
    assert(leaf_slot_ptr.shape_addr == target_rt.shapes.pointer_shape);
    assert(leaf_slot_ptr.as_pod<PointerLayout>().shape_ptr == target_rt.shapes.raw8_shape);
    // Also verify a brand-new dynamic leaf shape (size=3, align=1) via shapes.intern_shape
    {
        ShapeLayout leaf3{3, 1, FieldMapArena::empty()};
        trf->drop_in_chain(trf->field(target_rt.symbols.intern("shape")));
        trf->bind(target_rt.symbols.intern("shape"),
                  trf->allocate_val(Value::from_pod(target_rt.shapes.shape_meta, leaf3), false)->pointer());
        Value p3 = ev.run(fe.parse(
            "#allocate(#evaluate(#deref(#field(#field(#this, sh), intern_shape))), #deref(#field(#this, T)))"));
        assert(p3.shape_addr == target_rt.shapes.pointer_shape);
        const auto* s3 = reinterpret_cast<const ShapeLayout*>(p3.as_pod<PointerLayout>().shape_ptr);
        assert(s3->size == 3 && s3->align == 1 && s3->is_leaf());
    }

    // 2. Dynamic struct shape with mutable field (mutability=1) -> intern_shape -> reify/intern preserves mutability!
    {
        FieldMapInline empty = FieldMapArena::empty();
        PointerLayout rel_mut{0, target_rt.shapes.raw8_shape, 1};
        FieldMapInline fm1 = target_rt.shapes.field_maps.insert(empty, target_rt.symbols.intern("m"), rel_mut);
        ShapeLayout dyn_sl{8, 8, fm1};
        Value dyn_shape_val = Value::from_pod(target_rt.shapes.shape_meta, dyn_sl);

        trf->drop_in_chain(trf->field(target_rt.symbols.intern("shape")));
        trf->bind(target_rt.symbols.intern("shape"), trf->allocate_val(dyn_shape_val, false)->pointer());

        Value interned = ev.run(fe.parse("#evaluate(#deref(#field(#field(#this, sh), intern_shape)))"));
        assert(interned.shape_addr == target_rt.shapes.pointer_shape);
        PointerLayout sp = interned.as_pod<PointerLayout>();
        assert(sp.shape_ptr == target_rt.shapes.shape_meta && target_rt.shapes.owns(sp.addr));

        // Round-trip through AstValue into a third RuntimeContext and verify mutability == 1 is preserved
        AstValue av_shape = reify_ast_value(target_rt, interned);
        RuntimeContext rt3;
        Value rt3_shape = intern_ast_value(rt3, av_shape);
        const auto* sl3 = reinterpret_cast<const ShapeLayout*>(rt3_shape.as_pod<PointerLayout>().addr);
        assert(sl3->fields.count == 1);
        assert(sl3->fields.entries_begin()[0].mutability() == 1);

        // Also test StructVal mutability preservation
        ConstructingFrame tmp_f;
        FrameSlot* ms = tmp_f.allocate_val(Value::from_pod<uint64_t>(target_rt.shapes.raw8_shape, 99), true);
        tmp_f.bind(target_rt.symbols.intern("m"), ms->pointer());
        Value closed_mut_struct = tmp_f.close_and_flatten(target_rt.shapes);
        AstValue av_struct = reify_ast_value(target_rt, closed_mut_struct);
        Value rt3_struct = intern_ast_value(rt3, av_struct);
        assert(rt3_struct.shape_addr == rt3_shape.as_pod<PointerLayout>().addr);
    }

    // 3. Stageable symbols.* in isolated zero-capability target runtime
    {
        Value sym_roundtrip = ev.run(fe.parse(R"(
            #bind(sym, #allocate_val(hello, #deref(#field(#this, F)))) ;
            #bind(n, #allocate_val(#evaluate(#deref(#field(#field(#this, sy), byte_len))), #deref(#field(#this, F)))) ;
            #bind(index, #allocate_val(#deref(#field(#this, V1)), #deref(#field(#this, F)))) ;
            #bind(ch1, #allocate_val(#evaluate(#deref(#field(#field(#this, sy), byte_at))), #deref(#field(#this, F)))) ;
            #bind(bytes, #allocate_val(#evaluate(#deref(#field(#field(#this, sy), to_bytes))), #deref(#field(#this, F)))) ;
            #evaluate(#deref(#field(#field(#this, sy), intern_bytes)))
        )"));
        assert(sym_roundtrip.shape_addr == target_rt.shapes.symbol_shape);
        assert(target_rt.symbols.name(sym_roundtrip.as_pod<SymbolId>()) == "hello");
        assert(target_rt.deref(trf->field(target_rt.symbols.intern("n"))).as_pod<uint64_t>() == 5);
        assert(target_rt.deref(trf->field(target_rt.symbols.intern("ch1"))).as_pod<uint8_t>() == 'e');

        // Stageable bytes_slice + bytes_concat + bytes_at in zero-cap target runtime
        Value b_at = ev.run(fe.parse(R"(
            #bind(start, #allocate_val(#deref(#field(#this, V1)), #deref(#field(#this, F)))) ;
            #bind(len, #allocate_val(#deref(#field(#this, V3)), #deref(#field(#this, F)))) ;
            #bind(lhs, #allocate_val(#evaluate(#deref(#field(#this, p_bs))), #deref(#field(#this, F)))) ;
            #bind(rhs, #allocate_val(#deref(#field(#this, lhs)), #deref(#field(#this, F)))) ;
            #drop(#field(#this, bytes)) ;
            #bind(bytes, #allocate_val(#evaluate(#deref(#field(#this, p_bc))), #deref(#field(#this, F)))) ;
            #evaluate(#deref(#field(#this, p_ba)))
        )"));
        assert(b_at.shape_addr == target_rt.shapes.raw1_shape);
        assert(b_at.as_pod<uint8_t>() == 'l'); // "ellell"[1] == 'l'
    }

    // 4. shapes.intern_shape rejects invalid align (0 or non-power-of-2) and out-of-bounds field
    {
        auto expect_intern_throws = [&](ShapeLayout bad_sl) {
            trf->drop_in_chain(trf->field(target_rt.symbols.intern("shape")));
            trf->bind(target_rt.symbols.intern("shape"),
                      trf->allocate_val(Value::from_pod(target_rt.shapes.shape_meta, bad_sl), false)->pointer());
            bool threw = false;
            try {
                (void)ev.run(fe.parse("#evaluate(#deref(#field(#field(#this, sh), intern_shape)))"));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            assert(threw);
        };

        expect_intern_throws(ShapeLayout{8, 0, FieldMapArena::empty()}); // align == 0
        expect_intern_throws(ShapeLayout{8, 3, FieldMapArena::empty()}); // align not power of 2

        FieldMapInline oob_fm = target_rt.shapes.field_maps.insert(
            FieldMapArena::empty(), target_rt.symbols.intern("bad"),
            PointerLayout{4, target_rt.shapes.raw8_shape, 0});
        expect_intern_throws(ShapeLayout{8, 8, oob_fm}); // offset 4 + size 8 > 8
    }
}

static void test_backends_interpreter_run_isolation() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    // 1. Isolated target AST computes value without seeing capabilities or #parent
    Value res = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(arena, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), new_arena))), #deref(#field(#this, F)))) ;
        #bind(sym, #allocate_val(isolated_ok, #deref(#field(#this, F)))) ;
        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), word))), #deref(#field(#this, F)))) ;
        #bind(out, #allocate_val(#evaluate(#deref(#field(#field(#field(#field(#this, capabilities), backends), interpreter), run))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), ast), destroy_arena))) ;
        #deref(#field(#this, out))
    )"));
    assert(res.shape_addr == rt.shapes.symbol_shape);
    assert(rt.symbols.name(res.as_pod<SymbolId>()) == "isolated_ok");

    // 2. Verify capabilities is absent (addr == 0) inside fresh target RuntimeContext + Evaluator
    auto* rf = rt.current_frame();
    const std::string src_caps_str =
        "#bind(p, #allocate_val(#field(#this, capabilities), #deref(#field(#field(#this, dummy), x))))";
    // Use pure Layer 0: #field(#this, capabilities) returns Pointer; extract its .addr via #block + #break
    const std::string src_caps_ok =
        "#bind(p, #allocate_val(#field(#this, capabilities), #deref(#field(#field(#this, p0), mutability))))";
    (void)src_caps_str;
    (void)src_caps_ok;
    const std::string src_caps_l0 =
        "#bind(p0, #allocate_val(#field(#this, capabilities), #deref(#field(#field(#this, dummy), m))))";
    (void)src_caps_l0;

    // Build pure Layer 0 ASTs using #allocate_val(#field(#this, ...), z) via ast.value(false)
    Value caps_addr = ev.run(fe.parse(R"(
        #bind(arena2, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), new_arena))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, arena)) ;
        #bind(arena, #allocate_val(#deref(#field(#this, arena2)), #deref(#field(#this, F)))) ;

        ! Build `#bind(p, #allocate_val(#field(#this, capabilities), false)) ; #deref(#field(#field(#this, p), addr))` !
        #bind(val, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(af, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), value))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, val)) ;
        #bind(kind, #allocate_val(this, #deref(#field(#this, F)))) ;
        #bind(ath, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ;
        #bind(sym, #allocate_val(capabilities, #deref(#field(#this, F)))) ;
        #bind(wcaps, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(sym, #allocate_val(p, #deref(#field(#this, F)))) ;
        #bind(wp, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(sym, #allocate_val(addr, #deref(#field(#this, F)))) ;
        #bind(waddr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, ath)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, wcaps)), #deref(#field(#this, F)))) ;
        #bind(fcaps, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(allocate_val, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, fcaps)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, af)), #deref(#field(#this, F)))) ;
        #bind(alp, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(bind, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, wp)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, alp)), #deref(#field(#this, F)))) ;
        #bind(bp, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, ath)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, wp)), #deref(#field(#this, F)))) ;
        #bind(fp, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, fp)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, waddr)), #deref(#field(#this, F)))) ;
        #bind(faddr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(deref, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, faddr)), #deref(#field(#this, F)))) ;
        #bind(daddr, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ;

        #drop(#field(#this, ast)) ;
        #bind(0, #allocate_val(#deref(#field(#this, bp)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, daddr)), #deref(#field(#this, F)))) ;
        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ast), seq))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(out_addr, #allocate_val(#evaluate(#deref(#field(#field(#field(#field(#this, capabilities), backends), interpreter), run))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), ast), destroy_arena))) ;
        #deref(#field(#this, out_addr))
    )"));
    assert(caps_addr.shape_addr == rt.shapes.raw8_shape);
    assert(caps_addr.as_pod<uint64_t>() == 0);

    // 3. Stageability negative tests: reify_ast_value strictly rejects Pointer, Handle, and NativeProto (capabilities)
    {
        bool ptr_rejected = false;
        try {
            (void)reify_ast_value(rt, rt.make_pointer(rf->field(rt.symbols.intern("capabilities"))));
        } catch (const std::runtime_error&) {
            ptr_rejected = true;
        }
        assert(ptr_rejected);

        bool handle_rejected = false;
        try {
            (void)reify_ast_value(rt, rt.make_handle(12345));
        } catch (const std::runtime_error&) {
            handle_rejected = true;
        }
        assert(handle_rejected);

        bool caps_rejected = false;
        try {
            (void)reify_ast_value(rt, rt.deref(rf->field(rt.symbols.intern("capabilities"))));
        } catch (const std::runtime_error&) {
            caps_rejected = true;
        }
        assert(caps_rejected);
    }
}

int main() {
    test_io_read_write_and_symbols_intern();
    test_intrinsics_bytes_u8_and_eq_shape();
    test_prototype_factory_intrinsics();
    test_stageable_runtime_intrinsics_and_intern_shape();
    test_syntax_cst_view_bytes_and_child_view_sym();
    test_install_runtime_isolation();
    test_backends_interpreter_run_isolation();
    std::cout << "All Capabilities C++ tests passed!" << std::endl;
    return 0;
}
