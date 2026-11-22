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
    CapabilityHost caps;
    caps.install(rt);
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
    CapabilityHost caps;
    caps.install(rt);
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
        #evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), u8_to_i64)))
    )"));
    assert(ch_i64.shape_addr == rt.shapes.raw8_shape);
    assert(ch_i64.as_pod<int64_t>() == 50); // ASCII '2'

    // eq_shape: shape(ch_i64) == raw8_shape -> true; shape(ch_i64) == boolean_shape -> false
    Value eq_true = ev.run(fe.parse(R"(
        #bind(a, #allocate_val(#shape(#deref(#field(#this, v1))), #deref(#field(#this, F)))) ;
        #bind(b_s, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), #deref(#field(#this, F)))) ;
        #drop(#field(#this, b)) ;
        #bind(b, #allocate_val(#deref(#field(#this, b_s)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), eq_shape)))
    )"));
    assert(eq_true.shape_addr == rt.shapes.boolean_shape);
    assert(eq_true.as_pod<uint8_t>() == 1);

    Value eq_false = ev.run(fe.parse(R"(
        #drop(#field(#this, b)) ;
        #bind(b, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), boolean_shape)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), eq_shape)))
    )"));
    assert(eq_false.shape_addr == rt.shapes.boolean_shape);
    assert(eq_false.as_pod<uint8_t>() == 0);
}

static void test_syntax_cst_view_bytes_and_child_view_sym() {
    RuntimeContext rt;
    CapabilityHost caps;
    caps.install(rt);
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
        #evaluate(#evaluate(#deref(#field(#field(#field(#this, capabilities), compiler), parse))))
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

        #evaluate(#evaluate(#deref(#field(#field(#field(#this, capabilities), compiler), parse))))
    )"));
    assert(child_sym.shape_addr == rt.shapes.symbol_shape);
    assert(rt.symbols.name(child_sym.as_pod<SymbolId>()) == "bar");
}

int main() {
    test_io_read_write_and_symbols_intern();
    test_intrinsics_bytes_u8_and_eq_shape();
    test_syntax_cst_view_bytes_and_child_view_sym();
    std::cout << "All Capabilities C++ tests passed!" << std::endl;
    return 0;
}
