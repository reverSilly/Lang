#include "../ast/Ast.h"
#include "../eval/Capabilities.h"
#include "../eval/Evaluator.h"
#include "../parser/Layer0.h"
#include "../runtime/Runtime.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace reverSilly::Lang::Ast;
using namespace reverSilly::Lang::Parser;
using namespace reverSilly::Lang::Runtime;
using namespace reverSilly::Lang::Eval;

static void test_ffi_strlen_named_abs_positional_and_close_lib() {
    RuntimeContext rt;
    install_runtime_with_ffi(rt);
    Evaluator ev(rt);
    Frontend fe;

    // Verify install_runtime_with_ffi mounts Tier 1 + Tier 2 (ffi) and excludes Tier 3
    ConstructingFrame* root = rt.current_frame();
    PointerLayout caps_ptr = root->field(rt.symbols.intern("capabilities"));
    assert(!rt.field(caps_ptr, rt.symbols.intern("ffi")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("syntax")).is_null());
    assert(rt.field(caps_ptr, rt.symbols.intern("ast")).is_null());

    const std::string empty_path = "";
    const std::string sym_strlen = "strlen";
    const std::string sym_abs = "abs";
    const std::string hello_cstr = std::string("hello_ffi", 9) + '\0';

    uint64_t strlen_args = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("s"), rt.shapes.raw8_shape, 0},
    });
    uint64_t abs_args = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("0"), rt.shapes.raw4_shape, 0},
    });

    auto make_abi = [&](uint64_t args_s, uint64_t ret_s) {
        ConstructingFrame tmp;
        auto* s_args = tmp.allocate_val(
            Value::from_pod(rt.shapes.shape_meta, *reinterpret_cast<const ShapeLayout*>(args_s)), false);
        tmp.bind(rt.symbols.intern("args"), s_args->pointer());
        auto* s_ret = tmp.allocate_val(
            Value::from_pod(rt.shapes.shape_meta, *reinterpret_cast<const ShapeLayout*>(ret_s)), false);
        tmp.bind(rt.symbols.intern("ret"), s_ret->pointer());
        auto* s_cc = tmp.allocate_val(rt.make_symbol(rt.symbols.intern("c")), false);
        tmp.bind(rt.symbols.intern("cc"), s_cc->pointer());
        return tmp.close_and_flatten(rt.shapes);
    };

    root->bind(rt.symbols.intern("empty_path"),
               root->allocate_val(rt.make_bytes_ptr(empty_path.data(), empty_path.size()), false)->pointer());
    root->bind(rt.symbols.intern("sym_strlen"),
               root->allocate_val(rt.make_bytes_ptr(sym_strlen.data(), sym_strlen.size()), false)->pointer());
    root->bind(rt.symbols.intern("sym_abs"),
               root->allocate_val(rt.make_bytes_ptr(sym_abs.data(), sym_abs.size()), false)->pointer());
    root->bind(rt.symbols.intern("abi_strlen"),
               root->allocate_val(make_abi(strlen_args, rt.shapes.raw8_shape), false)->pointer());
    root->bind(rt.symbols.intern("abi_abs"),
               root->allocate_val(make_abi(abs_args, rt.shapes.raw4_shape), false)->pointer());
    root->bind(rt.symbols.intern("hello_ptr"),
               root->allocate_val(rt.make_bytes_ptr(hello_cstr.data(), hello_cstr.size()), false)->pointer());
    root->bind(rt.symbols.intern("neg42"),
               root->allocate_val(Value::from_pod<int32_t>(rt.shapes.raw4_shape, -42), false)->pointer());

    // 1. Named argument call: strlen(s = hello_ptr.addr) -> 9
    Value len_val = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(path, #allocate_val(#deref(#field(#this, empty_path)), #deref(#field(#this, F)))) ;
        #bind(lib, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), open_lib))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, path)) ;

        #bind(name, #allocate_val(#deref(#field(#this, sym_strlen)), #deref(#field(#this, F)))) ;
        #bind(abi, #allocate_val(#deref(#field(#this, abi_strlen)), #deref(#field(#this, F)))) ;
        #bind(c_strlen, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), load_symbol))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, abi)) ;

        #bind(s, #allocate_val(#deref(#field(#field(#this, hello_ptr), addr)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, c_strlen)))
    )"));
    assert(len_val.shape_addr == rt.shapes.raw8_shape);
    assert(len_val.as_pod<uint64_t>() == 9);

    // Verify FfiBinding metadata is preserved on PrototypeObject for Phase 2 LLVM AOT
    Value proto_v = rt.deref(root->field(rt.symbols.intern("c_strlen")));
    const PrototypeObject* pobj = rt.prototypes.get(proto_v.as_pod<uint64_t>());
    assert(pobj->kind == CodeKind::NativeSym);
    assert(pobj->ffi_binding.has_value());
    assert(pobj->ffi_binding->symbol_name == "strlen");
    assert(pobj->ffi_binding->args_shape_addr == strlen_args);
    assert(pobj->ffi_binding->ret_shape_addr == rt.shapes.raw8_shape);

    // 2. Positional argument call with 32-bit int return: abs(0 = -42) -> 42
    Value abs_val = ev.run(fe.parse(R"(
        #bind(name, #allocate_val(#deref(#field(#this, sym_abs)), #deref(#field(#this, F)))) ;
        #bind(abi, #allocate_val(#deref(#field(#this, abi_abs)), #deref(#field(#this, F)))) ;
        #bind(c_abs, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), load_symbol))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, abi)) ;

        #bind(0, #allocate_val(#deref(#field(#this, neg42)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, c_abs)))
    )"));
    assert(abs_val.shape_addr == rt.shapes.raw4_shape);
    assert(abs_val.as_pod<int32_t>() == 42);

    // 3. Explicit manual close_lib -> true
    Value closed = ev.run(fe.parse(R"(
        #evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), close_lib)))
    )"));
    assert(closed.shape_addr == rt.shapes.raw1_shape);
    assert(closed.as_pod<uint8_t>() == 1);
}

static void test_ffi_f64_tag_struct_by_value_and_layout_check() {
    RuntimeContext rt;
    install_runtime_with_ffi(rt);
    Evaluator ev(rt);
    Frontend fe;

    uint64_t f64_shape = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("__raw"), rt.shapes.raw8_shape, 0},
        {rt.symbols.intern("__f64_tag"), rt.shapes.unit_shape, 0},
    });
    uint64_t div_t_shape = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("quot"), rt.shapes.raw4_shape, 0},
        {rt.symbols.intern("rem"), rt.shapes.raw4_shape, 0},
    });
    // Manually packed struct (non-C-ABI offsets: 1B @0 + 4B @1, size=5) -> must be rejected by load_symbol!
    uint64_t bad_packed_shape = rt.shapes.intern_struct(
        {
            FieldEntry{rt.symbols.intern("a"), static_cast<uint8_t>(SlotState::Live), {0, 0, 0},
                       PointerLayout{0, rt.shapes.raw1_shape, 0}},
            FieldEntry{rt.symbols.intern("b"), static_cast<uint8_t>(SlotState::Live), {0, 0, 0},
                       PointerLayout{1, rt.shapes.raw4_shape, 0}},
        },
        5, 1);

    uint64_t strtod_args = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("nptr"), rt.shapes.raw8_shape, 0},
        {rt.symbols.intern("endptr"), rt.shapes.raw8_shape, 0},
    });
    uint64_t div_args = rt.shapes.intern_struct_auto({
        {rt.symbols.intern("numer"), rt.shapes.raw4_shape, 0},
        {rt.symbols.intern("denom"), rt.shapes.raw4_shape, 0},
    });

    auto make_abi = [&](uint64_t args_s, uint64_t ret_s) {
        ConstructingFrame tmp;
        auto* s_args = tmp.allocate_val(
            Value::from_pod(rt.shapes.shape_meta, *reinterpret_cast<const ShapeLayout*>(args_s)), false);
        tmp.bind(rt.symbols.intern("args"), s_args->pointer());
        auto* s_ret = tmp.allocate_val(
            Value::from_pod(rt.shapes.shape_meta, *reinterpret_cast<const ShapeLayout*>(ret_s)), false);
        tmp.bind(rt.symbols.intern("ret"), s_ret->pointer());
        auto* s_cc = tmp.allocate_val(rt.make_symbol(rt.symbols.intern("c")), false);
        tmp.bind(rt.symbols.intern("cc"), s_cc->pointer());
        return tmp.close_and_flatten(rt.shapes);
    };

    const std::string empty_path = "";
    const std::string sym_strtod = "strtod";
    const std::string sym_div = "div";
    const std::string pi_cstr = std::string("3.14159", 7) + '\0';

    ConstructingFrame* root = rt.current_frame();
    root->bind(rt.symbols.intern("empty_path"),
               root->allocate_val(rt.make_bytes_ptr(empty_path.data(), empty_path.size()), false)->pointer());
    root->bind(rt.symbols.intern("sym_strtod"),
               root->allocate_val(rt.make_bytes_ptr(sym_strtod.data(), sym_strtod.size()), false)->pointer());
    root->bind(rt.symbols.intern("sym_div"),
               root->allocate_val(rt.make_bytes_ptr(sym_div.data(), sym_div.size()), false)->pointer());
    root->bind(rt.symbols.intern("abi_strtod"),
               root->allocate_val(make_abi(strtod_args, f64_shape), false)->pointer());
    root->bind(rt.symbols.intern("abi_div"),
               root->allocate_val(make_abi(div_args, div_t_shape), false)->pointer());
    root->bind(rt.symbols.intern("abi_bad_packed"),
               root->allocate_val(make_abi(div_args, bad_packed_shape), false)->pointer());
    root->bind(rt.symbols.intern("pi_ptr"),
               root->allocate_val(rt.make_bytes_ptr(pi_cstr.data(), pi_cstr.size()), false)->pointer());
    root->bind(rt.symbols.intern("v17"),
               root->allocate_val(Value::from_pod<int32_t>(rt.shapes.raw4_shape, 17), false)->pointer());
    root->bind(rt.symbols.intern("v5"),
               root->allocate_val(Value::from_pod<int32_t>(rt.shapes.raw4_shape, 5), false)->pointer());

    // 1. Float return via __f64_tag: strtod("3.14159", NULL) -> 3.14159
    Value pi_val = ev.run(fe.parse(R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(Z, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), u64_zero)), #deref(#field(#this, F)))) ;
        #bind(path, #allocate_val(#deref(#field(#this, empty_path)), #deref(#field(#this, F)))) ;
        #bind(lib, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), open_lib))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, path)) ;

        #bind(name, #allocate_val(#deref(#field(#this, sym_strtod)), #deref(#field(#this, F)))) ;
        #bind(abi, #allocate_val(#deref(#field(#this, abi_strtod)), #deref(#field(#this, F)))) ;
        #bind(c_strtod, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), load_symbol))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, abi)) ;

        #bind(nptr, #allocate_val(#deref(#field(#field(#this, pi_ptr), addr)), #deref(#field(#this, F)))) ;
        #bind(endptr, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, c_strtod)))
    )"));
    assert(pi_val.shape_addr == f64_shape);
    assert(std::fabs(pi_val.as_pod<double>() - 3.14159) < 1e-9);

    // 2. Composite C struct return by value: div(17, 5) -> { quot = 3, rem = 2 }
    Value div_val = ev.run(fe.parse(R"(
        #bind(name, #allocate_val(#deref(#field(#this, sym_div)), #deref(#field(#this, F)))) ;
        #bind(abi, #allocate_val(#deref(#field(#this, abi_div)), #deref(#field(#this, F)))) ;
        #bind(c_div, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), load_symbol))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, abi)) ;

        #bind(numer, #allocate_val(#deref(#field(#this, v17)), #deref(#field(#this, F)))) ;
        #bind(denom, #allocate_val(#deref(#field(#this, v5)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, c_div)))
    )"));
    assert(div_val.shape_addr == div_t_shape);
    auto* d_slot = root->allocate_val(div_val, false);
    int32_t quot = rt.deref(rt.field(d_slot->pointer(), rt.symbols.intern("quot"))).as_pod<int32_t>();
    int32_t rem = rt.deref(rt.field(d_slot->pointer(), rt.symbols.intern("rem"))).as_pod<int32_t>();
    assert(quot == 3 && rem == 2);

    // 3. Shape mismatch on argument must throw
    bool arg_threw = false;
    try {
        (void)ev.run(fe.parse(R"(
            #drop(#field(#this, numer)) ;
            #bind(numer, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
            #evaluate(#deref(#field(#this, c_div)))
        )"));
    } catch (const std::runtime_error&) {
        arg_threw = true;
    }
    assert(arg_threw);

    // 4. Non-C-ABI packed struct layout must be rejected at load_symbol time
    bool layout_threw = false;
    try {
        (void)ev.run(fe.parse(R"(
            #bind(name, #allocate_val(#deref(#field(#this, sym_div)), #deref(#field(#this, F)))) ;
            #bind(abi, #allocate_val(#deref(#field(#this, abi_bad_packed)), #deref(#field(#this, F)))) ;
            #evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), load_symbol)))
        )"));
    } catch (const std::runtime_error&) {
        layout_threw = true;
    }
    assert(layout_threw);

    (void)ev.run(fe.parse(R"(
        #evaluate(#deref(#field(#field(#field(#this, capabilities), ffi), close_lib)))
    )"));
}

int main() {
    test_ffi_strlen_named_abs_positional_and_close_lib();
    test_ffi_f64_tag_struct_by_value_and_layout_check();
    std::cout << "All FFI C++ tests passed!" << std::endl;
    return 0;
}
