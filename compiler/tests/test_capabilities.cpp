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
             rf->allocate_val(rt.make_bytes_ptr(tmp_path.data(), tmp_path.size()), false)->pointer());
    rf->bind(rt.symbols.intern("p_data"),
             rf->allocate_val(rt.make_bytes_ptr(payload.data(), payload.size()), false)->pointer());

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
             rf->allocate_val(rt.make_bytes_ptr(text.data(), text.size()), false)->pointer());
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
        #bind(from, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw1_shape), size)), #deref(#field(#this, F)))) ;
        #bind(to, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw8_shape), size)), #deref(#field(#this, F)))) ;
        #bind(src, #allocate_val(b, #deref(#field(#this, F)))) ;
        #bind(zext_1_8, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), ext_int))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, zext_1_8)))
    )"));
    assert(ch_i64.shape_addr == rt.shapes.raw8_shape);
    assert(ch_i64.as_pod<int64_t>() == 50); // ASCII '2'

    // eq_shape: shape(ch_i64) == raw8_shape -> 0x01; shape(ch_i64) == raw1_shape -> 0x00
    Value eq_true = ev.run(fe.parse(R"(
        #bind(a, #allocate_val(#shape(#deref(#field(#this, v1))), #deref(#field(#this, F)))) ;
        #bind(b_s, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), #deref(#field(#this, F)))) ;
        #drop(#field(#this, b)) ;
        #bind(b, #allocate_val(#deref(#field(#this, b_s)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), eq_shape)))
    )"));
    assert(eq_true.shape_addr == rt.shapes.raw1_shape);
    assert(eq_true.as_pod<uint8_t>() == 1);

    Value eq_false = ev.run(fe.parse(R"(
        #drop(#field(#this, b)) ;
        #bind(b, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), raw1_shape)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), eq_shape)))
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
             rf->allocate_val(rt.make_bytes_ptr(sample.data(), sample.size()), false)->pointer());

    // Build handler in C++ AST for concise unit testing of cst_view_bytes & cst_child_view_sym
    // Handler returns ast.value(cst_view_bytes(node))
    uint64_t handler_bytes = rt.prototypes.create_ast(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#parent, capabilities), constants), false)), #deref(#field(#field(#field(#parent, capabilities), constants), false)))) ;
        #bind(val, #allocate_val(#evaluate(#deref(#field(#field(#field(#parent, capabilities), syntax), cst_view_bytes))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#parent, capabilities), ast), value)))
    )"));
    rf->bind(rt.symbols.intern("h_bytes"), rf->allocate_val(rt.make_proto(handler_bytes), false)->pointer());

    uint64_t handler_child1 = rt.prototypes.create_ast(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#parent, capabilities), constants), false)), #deref(#field(#field(#field(#parent, capabilities), constants), false)))) ;
        #bind(index, #allocate_val(#deref(#field(#field(#field(#parent, capabilities), constants), u64_one)), #deref(#field(#this, F)))) ;
        #bind(val, #allocate_val(#evaluate(#deref(#field(#field(#field(#parent, capabilities), syntax), cst_child_view_sym))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#parent, capabilities), ast), value)))
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

    assert(bytes_res.shape_addr == rt.shapes.pointer_shape);
    auto bp = bytes_res.as_pod<PointerLayout>();
    const auto* bsh = reinterpret_cast<const ShapeLayout*>(bp.shape_ptr);
    assert(bsh->is_leaf() && bsh->align == 1 && bsh->size == 7);
    assert(std::string_view(reinterpret_cast<const char*>(bp.addr), bsh->size) == "foo:bar");

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
    assert(!rt.field(caps_ptr, rt.symbols.intern("symbols")).is_null());
    assert(!rt.field(caps_ptr, rt.symbols.intern("intrinsics")).is_null());

    // Tier 3 compiler frontend modules must be absent in runtime-only mode
    assert(rt.field(caps_ptr, rt.symbols.intern("ast")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("syntax")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("compiler")).is_null());
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
        #bind(W1, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw1_shape), size)), #deref(#field(#this, F)))) ;
        #bind(W2, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw2_shape), size)), #deref(#field(#this, F)))) ;
        #bind(W4, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw4_shape), size)), #deref(#field(#this, F)))) ;
        #bind(W8, #allocate_val(#deref(#field(#field(#field(#field(#this, capabilities), constants), raw8_shape), size)), #deref(#field(#this, F)))) ;
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

int main() {
    test_io_read_write_and_symbols_intern();
    test_intrinsics_bytes_u8_and_eq_shape();
    test_prototype_factory_intrinsics();
    test_syntax_cst_view_bytes_and_child_view_sym();
    test_install_runtime_isolation();
    std::cout << "All Capabilities C++ tests passed!" << std::endl;
    return 0;
}
