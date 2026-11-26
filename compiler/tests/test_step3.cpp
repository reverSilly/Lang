#include "../ast/Ast.h"
#include "../eval/Capabilities.h"
#include "../eval/Evaluator.h"
#include "../parser/Layer0.h"
#include "../runtime/Runtime.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace reverSilly::Lang::Ast;
using namespace reverSilly::Lang::Parser;
using namespace reverSilly::Lang::Runtime;
using namespace reverSilly::Lang::Eval;

static void test_pure_layer0_slots_and_intrinsics() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    auto root_frame = rt.current_frame();
    auto* s_10 = root_frame->allocate_val(Value::from_pod<int64_t>(rt.shapes.raw8_shape, 10), false);
    root_frame->bind(rt.symbols.intern("v10"), s_10->pointer());
    auto* s_32 = root_frame->allocate_val(Value::from_pod<int64_t>(rt.shapes.raw8_shape, 32), false);
    root_frame->bind(rt.symbols.intern("v32"), s_32->pointer());

    const char* src = R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(width, #allocate_val(#deref(#field(#deref(#field(#field(#field(#this, capabilities), constants), raw8_shape)), size)), #deref(#field(#this, F)))) ;
        #bind(lhs, #allocate_val(a, #deref(#field(#this, F)))) ;
        #bind(rhs, #allocate_val(b, #deref(#field(#this, F)))) ;
        #bind(add8, #allocate_val(#evaluate(#deref(#field(#field(#field(#this, capabilities), intrinsics), add_int))), #deref(#field(#this, F)))) ;
        #bind(a, #allocate_val(#deref(#field(#this, v10)), #deref(#field(#this, F)))) ;
        #bind(b, #allocate_val(#deref(#field(#this, v32)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#this, add8)))
    )";

    Value res = ev.run(fe.parse(src));
    assert(res.shape_addr == rt.shapes.raw8_shape);
    assert(res.as_pod<int64_t>() == 42);
}

static void test_block_flatten_break_and_branch() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    ExprPtr blk_body = fe.parse(R"(
        #bind(x, #allocate_val(hello, #deref(#field(#field(#field(#parent(#this), capabilities), constants), false)))) ;
        #bind(y, #allocate_val(world, #deref(#field(#field(#field(#parent(#this), capabilities), constants), false)))) ;
        #drop(#field(#this, y))
    )");
    uint64_t blk_proto = rt.prototypes.create_ast(blk_body);
    auto* p_slot = rt.current_frame()->allocate_val(rt.make_proto(blk_proto), false);
    rt.current_frame()->bind(rt.symbols.intern("blk_p"), p_slot->pointer());

    Value closed_ptr_val = ev.run(fe.parse(
        "#block(#deref(#field(#this, blk_p)), #deref(#field(#field(#field(#this, capabilities), constants), false)))"));
    assert(closed_ptr_val.shape_addr == rt.shapes.pointer_shape);
    PointerLayout closed_ptr = closed_ptr_val.as_pod<PointerLayout>();
    assert(!closed_ptr.is_null() && closed_ptr.mutability == 0);
    const ShapeLayout* cs = reinterpret_cast<const ShapeLayout*>(closed_ptr.shape_ptr);
    assert(cs->fields.count == 1);
    assert(cs->fields.entries_begin()[0].sym == rt.symbols.intern("x"));

    // Direct zero-copy #bind of unbound slot R_B returned by #block
    bool bound_ok = rt.current_frame()->bind(rt.symbols.intern("closed_obj"), closed_ptr);
    assert(bound_ok);
    (void)bound_ok;

    Value count_val = ev.run(fe.parse(R"(
        #bind(s, #allocate_val(#shape(#deref(#field(#this, closed_obj))), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(map, #allocate_val(#deref(#field(#deref(#field(#this, s)), fields)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #evaluate(#deref(#field(#field(#field(#this, capabilities), field_map), count)))
    )"));
    assert(count_val.as_pod<uint64_t>() == 1);

    ExprPtr brk_body = fe.parse("#break(early_exit) ; unreachable");
    uint64_t brk_proto = rt.prototypes.create_ast(brk_body);
    auto* bp_slot = rt.current_frame()->allocate_val(rt.make_proto(brk_proto), false);
    rt.current_frame()->bind(rt.symbols.intern("brk_p"), bp_slot->pointer());

    Value brk_ptr_val = ev.run(fe.parse(
        "#block(#deref(#field(#this, brk_p)), #deref(#field(#field(#field(#this, capabilities), constants), true)))"));
    assert(brk_ptr_val.shape_addr == rt.shapes.pointer_shape);
    PointerLayout brk_ptr = brk_ptr_val.as_pod<PointerLayout>();
    assert(!brk_ptr.is_null() && brk_ptr.mutability == 1 && brk_ptr.shape_ptr == rt.shapes.symbol_shape);
    Value brk_res = rt.deref(brk_ptr);
    assert(rt.symbols.name(brk_res.as_pod<SymbolId>()) == "early_exit");
    // Mutable R_B returned by #block(p, true) can be overwritten directly
    rt.write(brk_ptr, rt.make_symbol(rt.symbols.intern("mutated_exit")));
    assert(rt.symbols.name(rt.deref(brk_ptr).as_pod<SymbolId>()) == "mutated_exit");

    uint64_t p_yes = rt.prototypes.create_ast(fe.parse("yes_branch"));
    uint64_t p_no = rt.prototypes.create_ast(fe.parse("no_branch"));
    rt.current_frame()->bind(rt.symbols.intern("p_yes"), rt.current_frame()->allocate_val(rt.make_proto(p_yes), false)->pointer());
    rt.current_frame()->bind(rt.symbols.intern("p_no"), rt.current_frame()->allocate_val(rt.make_proto(p_no), false)->pointer());

    Value b_true = ev.run(fe.parse(
        "#branch(#deref(#field(#field(#field(#this, capabilities), constants), true)), "
        "#deref(#field(#this, p_yes)), #deref(#field(#this, p_no)))"));
    assert(rt.symbols.name(b_true.as_pod<SymbolId>()) == "yes_branch");

    // Multi-level #parent(#parent(#this)) + null on root frame and non-open-frame pointers
    uint64_t inner2_p = rt.prototypes.create_ast(fe.parse(
        "#break(#deref(#field(#parent(#parent(#this)), outer_tag)))"));
    uint64_t mid_p = rt.prototypes.create_ast(make_expr<BuiltinExpr>(
        BuiltinKind::Break,
        std::vector<ExprPtr>{make_expr<BuiltinExpr>(
            BuiltinKind::Deref,
            std::vector<ExprPtr>{make_expr<BuiltinExpr>(
                BuiltinKind::Block,
                std::vector<ExprPtr>{
                    make_expr<ValueExpr>(reify_ast_value(rt, rt.make_proto(inner2_p))),
                    make_expr<ValueExpr>(reify_ast_value(rt, Value::from_pod<uint8_t>(rt.shapes.raw1_shape, 0))),
                })})}));
    rt.current_frame()->bind(
        rt.symbols.intern("outer_tag"),
        rt.current_frame()->allocate_val(rt.make_symbol(rt.symbols.intern("grandparent_ok")), false)->pointer());
    rt.current_frame()->bind(
        rt.symbols.intern("mid_p"),
        rt.current_frame()->allocate_val(rt.make_proto(mid_p), false)->pointer());

    Value gp_ptr_val = ev.run(fe.parse(
        "#block(#deref(#field(#this, mid_p)), #deref(#field(#field(#field(#this, capabilities), constants), false)))"));
    assert(gp_ptr_val.shape_addr == rt.shapes.pointer_shape);
    Value gp_res = rt.deref(gp_ptr_val.as_pod<PointerLayout>());
    assert(gp_res.shape_addr == rt.shapes.symbol_shape);
    assert(rt.symbols.name(gp_res.as_pod<SymbolId>()) == "grandparent_ok");

    Value root_par = ev.run(fe.parse("#parent(#this)"));
    assert(root_par.as_pod<PointerLayout>().is_null());

    // Empty closed unit struct also has size == 0, must NOT be misidentified as an OpenFrame
    rt.current_frame()->bind(
        rt.symbols.intern("u_slot"),
        rt.current_frame()->allocate_val(rt.make_unit(), false)->pointer());
    Value non_frame_par = ev.run(fe.parse("#parent(#field(#this, u_slot))"));
    assert(non_frame_par.as_pod<PointerLayout>().is_null());
}

static void test_coroutines_unified_frame_reflection_and_destroy() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    assert(rt.coroutines.live_count() == 1);

    // Worker coroutine:
    // 1. Binds `secret` in its root frame
    // 2. Enters a nested #block, binds `nested_var`, then yields `first_reply` back to #caller
    // 3. Upon second resume, exits via root #break(#deref(#in))
    ExprPtr inner_body = make_expr<SequenceExpr>(std::vector<ExprPtr>{
        make_expr<BuiltinExpr>(
            BuiltinKind::Bind,
            std::vector<ExprPtr>{
                make_expr<WordExpr>("nested_var"),
                make_expr<BuiltinExpr>(
                    BuiltinKind::AllocateVal,
                    std::vector<ExprPtr>{make_expr<WordExpr>("inner_val"), make_expr<ValueExpr>(reify_ast_value(rt, Value::from_pod<uint8_t>(rt.shapes.raw1_shape, 0)))}),
            }),
        fe.parse("#resume(#caller, first_reply)"),
    });
    uint64_t inner_proto = rt.prototypes.create_ast(inner_body);

    ExprPtr worker_ast = make_expr<SequenceExpr>(std::vector<ExprPtr>{
        make_expr<BuiltinExpr>(
            BuiltinKind::Bind,
            std::vector<ExprPtr>{
                make_expr<WordExpr>("secret"),
                make_expr<BuiltinExpr>(
                    BuiltinKind::AllocateVal,
                    std::vector<ExprPtr>{make_expr<WordExpr>("root_val"), make_expr<ValueExpr>(reify_ast_value(rt, Value::from_pod<uint8_t>(rt.shapes.raw1_shape, 0)))}),
            }),
        make_expr<BuiltinExpr>(
            BuiltinKind::Block,
            std::vector<ExprPtr>{
                make_expr<ValueExpr>(reify_ast_value(rt, rt.make_proto(inner_proto))),
                make_expr<ValueExpr>(reify_ast_value(rt, Value::from_pod<uint8_t>(rt.shapes.raw1_shape, 0))),
            }),
        fe.parse("#break(#deref(#in))"),
    });

    uint64_t worker_proto = rt.prototypes.create_ast(worker_ast);
    rt.current_frame()->bind(
        rt.symbols.intern("worker_p"),
        rt.current_frame()->allocate_val(rt.make_proto(worker_proto), false)->pointer());

    // Resume #1: worker suspends INSIDE nested #block!
    // Verify #coro_root_frame(coro) exposes unified ShapeLayout containing:
    //   slot 0: `secret` (sym != 0, Live)
    //   slot 1: active child open frame (sym == 0, Live, sub_shape.size == 0) containing `nested_var`!
    Value root_ptr_val = ev.run(fe.parse(R"(
        #bind(coro, #allocate_val(#new(#deref(#field(#this, worker_p))), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #resume(#deref(#field(#this, coro)), first_in) ;
        #bind(r1, #allocate_val(#deref(#in), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #coro_root_frame(#deref(#field(#this, coro)))
    )"));

    PointerLayout root_fp = root_ptr_val.as_pod<PointerLayout>();
    assert(!root_fp.is_null() && root_fp.shape_ptr != 0);
    const auto* root_shape = reinterpret_cast<const ShapeLayout*>(root_fp.shape_ptr);
    assert(root_shape->size == 0); // open frame
    assert(root_shape->fields.count == 2); // `secret` + active child open frame!

    // Access `secret` directly via unified #field on #coro_root_frame!
    PointerLayout p_secret = rt.field(root_fp, rt.symbols.intern("secret"));
    assert(rt.symbols.name(rt.deref(p_secret).as_pod<SymbolId>()) == "root_val");

    // Descend into active child frame (slot 1: sym == 0, Live, sub_shape->size == 0) -> compute top_frame!
    FieldEntry child_fe = FieldMapArena::at(root_shape->fields, 1);
    assert(child_fe.sym == 0 && child_fe.slot_state() == SlotState::Live);
    PointerLayout child_fp{root_fp.addr + child_fe.offset(), child_fe.sub_shape_addr(), child_fe.mutability()};
    const auto* child_shape = reinterpret_cast<const ShapeLayout*>(child_fp.shape_ptr);
    assert(child_shape->size == 0 && child_shape->fields.count == 1);
    PointerLayout p_nested = rt.field(child_fp, rt.symbols.intern("nested_var"));
    assert(rt.symbols.name(rt.deref(p_nested).as_pod<SymbolId>()) == "inner_val");

    // Resume #2 -> worker finishes via root #break(#deref(#in)) and is automatically reclaimed!
    Value final_val = ev.run(fe.parse(R"(
        #resume(#deref(#field(#this, coro)), second_in) ;
        #deref(#in)
    )"));
    assert(rt.symbols.name(final_val.as_pod<SymbolId>()) == "second_in");
    assert(rt.coroutines.live_count() == 1);

    // Core #destroy(coro) on an abandoned suspended coroutine
    Value destroyed = ev.run(fe.parse(R"(
        #bind(abandoned, #allocate_val(#new(#deref(#field(#this, worker_p))), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #destroy(#deref(#field(#this, abandoned)))
    )"));
    assert(destroyed.as_pod<uint8_t>() == 1);
    assert(rt.coroutines.live_count() == 1);
}

static void test_zero_caret_layer0_bootstrap_of_caret_syntax() {
    RuntimeContext rt;
    install_all_capabilities(rt);
    Evaluator ev(rt);
    Frontend fe;

    const std::string target_src =
        "#block(^#bind(x, #allocate_val(hello, #deref(#field(#field(#field(#parent(#this), capabilities), constants), false)))), "
        "#deref(#field(#field(#field(#this, capabilities), constants), false)))";
    rt.current_frame()->bind(
        rt.symbols.intern("target_src"),
        rt.current_frame()->allocate_val(rt.make_raw_bytes(target_src.data(), target_src.size()), false)->pointer());

    const char* bootstrap_src = R"(
        #bind(F, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), false)), #deref(#field(#field(#field(#this, capabilities), constants), false)))) ;
        #bind(T, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), true)), #deref(#field(#this, F)))) ;
        #bind(Z, #allocate_val(#deref(#field(#field(#field(#this, capabilities), constants), u64_zero)), #deref(#field(#this, F)))) ;
        #bind(caps, #allocate_val(#deref(#field(#this, capabilities)), #deref(#field(#this, F)))) ;
        #bind(c_ast, #allocate_val(#deref(#field(#field(#this, caps), ast)), #deref(#field(#this, F)))) ;
        #bind(c_syn, #allocate_val(#deref(#field(#field(#this, caps), syntax)), #deref(#field(#this, F)))) ;

        #bind(fe, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), new_layer0))), #deref(#field(#this, F)))) ;
        #bind(arena, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), new_arena))), #deref(#field(#this, F)))) ;

        #bind(val, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(n_F, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), value))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, val)) ;

        #bind(val, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #bind(n_Z, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), value))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, val)) ;

        #bind(kind, #allocate_val(this, #deref(#field(#this, F)))) ;
        #bind(t3_this, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ;
        #bind(sym, #allocate_val(capabilities, #deref(#field(#this, F)))) ;
        #bind(w_c, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, t3_this)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_c)), #deref(#field(#this, F)))) ;
        #bind(p_c, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(sym, #allocate_val(syntax, #deref(#field(#this, F)))) ;
        #bind(w_s, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_c)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_s)), #deref(#field(#this, F)))) ;
        #bind(p_s, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(sym, #allocate_val(ast, #deref(#field(#this, F)))) ;
        #bind(w_a, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_c)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_a)), #deref(#field(#this, F)))) ;
        #bind(p_a, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(sym, #allocate_val(cst_eval_child, #deref(#field(#this, F)))) ;
        #bind(w_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_s)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_t)), #deref(#field(#this, F)))) ;
        #bind(p_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, w_t)) ;
        #bind(kind, #allocate_val(deref, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_t)), #deref(#field(#this, F)))) ;
        #bind(n_eval_child_fn, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, p_t)) ;

        #bind(sym, #allocate_val(to_proto, #deref(#field(#this, F)))) ;
        #bind(w_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_a)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_t)), #deref(#field(#this, F)))) ;
        #bind(p_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, w_t)) ;
        #bind(kind, #allocate_val(deref, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_t)), #deref(#field(#this, F)))) ;
        #bind(n_to_proto_fn, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, p_t)) ;

        #bind(sym, #allocate_val(value, #deref(#field(#this, F)))) ;
        #bind(w_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;
        #bind(kind, #allocate_val(field, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_a)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, w_t)), #deref(#field(#this, F)))) ;
        #bind(p_t, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, w_t)) ;
        #bind(kind, #allocate_val(deref, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, p_t)), #deref(#field(#this, F)))) ;
        #bind(n_ast_val_fn, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, p_t)) ;

        #bind(sym, #allocate_val(index, #deref(#field(#this, F)))) ;
        #bind(w_index, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(kind, #allocate_val(allocate_val, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, n_Z)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, n_F)), #deref(#field(#this, F)))) ;
        #bind(alloc_z, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(bind, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, w_index)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, alloc_z)), #deref(#field(#this, F)))) ;
        #bind(stmt1, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(sym, #allocate_val(ast, #deref(#field(#this, F)))) ;
        #bind(w_ast, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(kind, #allocate_val(evaluate, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, n_eval_child_fn)), #deref(#field(#this, F)))) ;
        #bind(call_ec, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ;

        #bind(kind, #allocate_val(allocate_val, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, call_ec)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, n_F)), #deref(#field(#this, F)))) ;
        #bind(alloc_ec, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(bind, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, w_ast)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, alloc_ec)), #deref(#field(#this, F)))) ;
        #bind(stmt2, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(sym, #allocate_val(val, #deref(#field(#this, F)))) ;
        #bind(w_val, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), word))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(kind, #allocate_val(evaluate, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, n_to_proto_fn)), #deref(#field(#this, F)))) ;
        #bind(call_tp, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ;

        #bind(kind, #allocate_val(allocate_val, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, call_tp)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, n_F)), #deref(#field(#this, F)))) ;
        #bind(alloc_tp, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(bind, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, w_val)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, alloc_tp)), #deref(#field(#this, F)))) ;
        #bind(stmt3, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(kind, #allocate_val(evaluate, #deref(#field(#this, F)))) ;
        #bind(0, #allocate_val(#deref(#field(#this, n_ast_val_fn)), #deref(#field(#this, F)))) ;
        #bind(stmt4, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), builtin))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, kind)) ; #drop(#field(#this, 0)) ;

        #bind(0, #allocate_val(#deref(#field(#this, stmt1)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, stmt2)), #deref(#field(#this, F)))) ;
        #bind(2, #allocate_val(#deref(#field(#this, stmt3)), #deref(#field(#this, F)))) ;
        #bind(3, #allocate_val(#deref(#field(#this, stmt4)), #deref(#field(#this, F)))) ;
        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), seq))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, 2)) ; #drop(#field(#this, 3)) ;
        #bind(caret_visitor, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), to_proto))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, ast)) ;

        #bind(sym, #allocate_val(^, #deref(#field(#this, F)))) ;
        #bind(lit_caret, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), lit))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, sym)) ;

        #bind(name, #allocate_val(Delimiter, #deref(#field(#this, F)))) ;
        #bind(old_delim, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), get_rule))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, old_delim)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, lit_caret)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), choice))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(name, #allocate_val(Delimiter, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, T)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#this, c_syn), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(name, #allocate_val(_, #deref(#field(#this, F)))) ;
        #bind(ref_ws, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), ref))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(name, #allocate_val(Prefix, #deref(#field(#this, F)))) ;
        #bind(ref_prefix, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), ref))), #deref(#field(#this, F)))) ;
        #bind(old_prefix, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), get_rule))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, lit_caret)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, ref_ws)), #deref(#field(#this, F)))) ;
        #bind(2, #allocate_val(#deref(#field(#this, ref_prefix)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), seq))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ; #drop(#field(#this, 2)) ;

        #bind(name, #allocate_val(Proto, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, caret_visitor)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#this, c_syn), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(name, #allocate_val(Proto, #deref(#field(#this, F)))) ;
        #bind(ref_proto, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), ref))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, name)) ;

        #bind(0, #allocate_val(#deref(#field(#this, ref_proto)), #deref(#field(#this, F)))) ;
        #bind(1, #allocate_val(#deref(#field(#this, old_prefix)), #deref(#field(#this, F)))) ;
        #bind(expr, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), choice))), #deref(#field(#this, F)))) ;
        #drop(#field(#this, 0)) ; #drop(#field(#this, 1)) ;

        #bind(name, #allocate_val(Prefix, #deref(#field(#this, F)))) ;
        #bind(hidden, #allocate_val(#deref(#field(#this, F)), #deref(#field(#this, F)))) ;
        #bind(handler, #allocate_val(#deref(#field(#this, Z)), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#this, c_syn), set_rule))) ;
        #drop(#field(#this, name)) ; #drop(#field(#this, expr)) ; #drop(#field(#this, hidden)) ; #drop(#field(#this, handler)) ;

        #bind(src, #allocate_val(#deref(#field(#this, target_src)), #deref(#field(#this, F)))) ;
        #bind(ast, #allocate_val(#evaluate(#deref(#field(#field(#this, c_syn), parse))), #deref(#field(#this, F)))) ;
        #bind(parsed_proto, #allocate_val(#evaluate(#deref(#field(#field(#this, c_ast), to_proto))), #deref(#field(#this, F)))) ;
        #evaluate(#deref(#field(#field(#this, c_ast), destroy_arena))) ;
        #evaluate(#deref(#field(#field(#this, c_syn), destroy_frontend))) ;
        #evaluate(#deref(#field(#this, parsed_proto)))
    )";

    Value result_ptr_val = ev.run(fe.parse(bootstrap_src));
    assert(result_ptr_val.shape_addr == rt.shapes.pointer_shape);
    PointerLayout r_ptr = result_ptr_val.as_pod<PointerLayout>();
    const ShapeLayout* s = reinterpret_cast<const ShapeLayout*>(r_ptr.shape_ptr);
    assert(s->fields.count == 1);
    assert(s->fields.entries_begin()[0].sym == rt.symbols.intern("x"));

    PointerLayout px = rt.field(r_ptr, rt.symbols.intern("x"));
    Value x_val = rt.deref(px);
    assert(rt.symbols.name(x_val.as_pod<SymbolId>()) == "hello");
}

int main() {
    test_pure_layer0_slots_and_intrinsics();
    test_block_flatten_break_and_branch();
    test_coroutines_unified_frame_reflection_and_destroy();
    test_zero_caret_layer0_bootstrap_of_caret_syntax();
    std::cout << "All Step 3 tests passed!" << std::endl;
    return 0;
}
