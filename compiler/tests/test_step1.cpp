#include "../ast/Ast.h"
#include "../runtime/Runtime.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace reverSilly::Lang::Runtime;
using namespace reverSilly::Lang::Ast;

static void test_symbol_pool() {
    SymbolPool pool;
    SymbolId a1 = pool.intern("alpha");
    SymbolId b1 = pool.intern("beta");
    SymbolId a2 = pool.intern("alpha");
    assert(a1 == a2);
    assert(a1 != b1);
    assert(pool.name(a1) == "alpha");
    assert(pool.name(b1) == "beta");
    assert(pool.size() == 2);
}

static void test_field_map_small_and_large_open_addressing() {
    RuntimeContext rt;
    auto& fm = rt.shapes.field_maps;

    FieldMapInline m = FieldMapArena::empty();
    assert(FieldMapArena::is_empty(m));
    assert(FieldMapArena::count(m) == 0);
    assert(FieldMapArena::get(m, rt.symbols.intern("x")).is_null());

    // Build a 20-field immutable map via persistent insert (triggers open-addressing buckets)
    for (uint32_t i = 0; i < 20; ++i) {
        SymbolId sym = rt.symbols.intern("f_" + std::to_string(i));
        m = fm.insert(m, sym, PointerLayout{i * 8ULL, rt.shapes.raw8_shape, static_cast<uint8_t>(i % 2)});
    }
    assert(!FieldMapArena::is_empty(m));
    assert(FieldMapArena::count(m) == 20);
    assert(m.buckets_ptr != 0);
    assert(m.bucket_mask == 63); // next_pow2(20 * 2) - 1 = 64 - 1

    // Verify O(1) lookup and definition-order forward/reverse iteration
    for (uint32_t i = 0; i < 20; ++i) {
        SymbolId sym = rt.symbols.intern("f_" + std::to_string(i));
        PointerLayout p = FieldMapArena::get(m, sym);
        assert(!p.is_null() && p.addr == i * 8ULL && p.shape_ptr == rt.shapes.raw8_shape);
        FieldEntry fe = FieldMapArena::at(m, i);
        assert(fe.sym == sym && fe.offset() == i * 8ULL);
    }
    assert(FieldMapArena::get(m, rt.symbols.intern("missing")).is_null());
}

static void test_shape_bootstrap_and_interning() {
    RuntimeContext rt;
    auto& s = rt.shapes;

    const auto* shape_meta = reinterpret_cast<const ShapeLayout*>(s.shape_meta);
    assert(shape_meta->size == sizeof(ShapeLayout)); // 40B
    assert(shape_meta->align == 8);
    assert(!shape_meta->is_leaf());
    assert(shape_meta->fields.count == 4); // size, align, fields, __shape_tag
    assert(shape_meta->fields.buckets_ptr == 0); // <= 8 fields => linear scan

    const auto* ptr_shape = reinterpret_cast<const ShapeLayout*>(s.pointer_shape);
    assert(ptr_shape->size == sizeof(PointerLayout)); // 24B
    assert(ptr_shape->align == 8);

    const auto* fe_shape = reinterpret_cast<const ShapeLayout*>(s.field_entry_shape);
    assert(fe_shape->size == sizeof(FieldEntry)); // 32B
    assert(fe_shape->align == 8);

    // Leaf Shape interning & ZST marker distinction
    uint64_t leaf4_a = s.intern_leaf(4, 4);
    assert(leaf4_a == s.raw4_shape);
    assert(reinterpret_cast<const ShapeLayout*>(leaf4_a)->is_leaf());

    uint64_t i32_shape = s.intern_struct_auto({
        {rt.symbols.intern("__raw"), s.raw4_shape, 0},
        {rt.symbols.intern("__i32_tag"), s.unit_shape, 0},
    });
    uint64_t u32_shape = s.intern_struct_auto({
        {rt.symbols.intern("__raw"), s.raw4_shape, 0},
        {rt.symbols.intern("__u32_tag"), s.unit_shape, 0},
    });
    assert(i32_shape != u32_shape);
    assert(reinterpret_cast<const ShapeLayout*>(i32_shape)->size == 4);

    // Verify z_ptr (addr=0) relative fat-pointer reflection via #field
    PointerLayout z_ptr{0, s.pointer_shape, 1};
    PointerLayout rel_shape_ptr = s.field_of(z_ptr, rt.symbols.intern("shape_ptr"));
    assert(rel_shape_ptr.addr == 8);
    assert(rel_shape_ptr.shape_ptr == s.raw8_shape);
    assert(rel_shape_ptr.mutability == 0);
}

static void test_frame_three_state_and_flatten() {
    RuntimeContext rt;
    ConstructingFrame* frame = rt.current_frame();

    SymbolId sym_x = rt.symbols.intern("x");
    SymbolId sym_y = rt.symbols.intern("y");
    SymbolId sym_tmp = rt.symbols.intern("tmp");
    SymbolId sym_uninit = rt.symbols.intern("uninit_slot");

    FrameSlot* slot_x = frame->allocate(rt.shapes.raw4_shape, false);
    assert(frame->bind(sym_x, slot_x->pointer()));
    PointerLayout px = frame->field(sym_x);

    bool threw = false;
    try { (void)rt.deref(px); } catch (...) { threw = true; }
    assert(threw);

    uint32_t v42 = 42;
    rt.write(px, Value::from_pod(rt.shapes.raw4_shape, v42));
    assert(rt.deref(px).as_pod<uint32_t>() == 42);

    threw = false;
    try { rt.write(px, Value::from_pod(rt.shapes.raw4_shape, v42)); } catch (...) { threw = true; }
    assert(threw);

    uint64_t v99 = 99;
    FrameSlot* slot_tmp = frame->allocate_val(Value::from_pod(rt.shapes.raw8_shape, v99), false);
    assert(frame->top_unbound().addr == slot_tmp->data_addr());
    assert(frame->bind(sym_tmp, slot_tmp->pointer()));
    assert(frame->top_unbound().is_null());
    frame->drop_in_chain(slot_tmp->pointer());

    FrameSlot* slot_u = frame->allocate(rt.shapes.raw8_shape, false);
    assert(frame->bind(sym_uninit, slot_u->pointer()));

    uint64_t v777 = 777;
    FrameSlot* slot_y = frame->allocate_val(Value::from_pod(rt.shapes.raw8_shape, v777), true);
    assert(frame->bind(sym_y, slot_y->pointer()));

    Value closed = frame->close_and_flatten(rt.shapes);
    const auto* cs = closed.shape();
    assert(cs->size == 16 && cs->align == 8 && cs->fields.count == 2);

    FrameSlot* closed_slot = frame->allocate_val(closed, true);
    PointerLayout p_closed = closed_slot->pointer();
    PointerLayout p_x = rt.shapes.field_of(p_closed, sym_x);
    PointerLayout p_y = rt.shapes.field_of(p_closed, sym_y);
    assert(rt.deref(p_x).as_pod<uint32_t>() == 42);
    assert(rt.deref(p_y).as_pod<uint64_t>() == 777);

    uint64_t v888 = 888;
    rt.write(p_y, Value::from_pod(rt.shapes.raw8_shape, v888));
    assert(rt.deref(p_y).as_pod<uint64_t>() == 888);

    threw = false;
    try { rt.write(p_x, Value::from_pod(rt.shapes.raw4_shape, v42)); } catch (...) { threw = true; }
    assert(threw);
}

int main() {
    test_symbol_pool();
    test_field_map_small_and_large_open_addressing();
    test_shape_bootstrap_and_interning();
    test_frame_three_state_and_flatten();
    std::cout << "All Step 1 tests passed!" << std::endl;
    return 0;
}
