#include "Capabilities.h"

#include <limits>
#include <stdexcept>

namespace reverSilly::Lang::Eval {

Runtime::Value CapabilityHost::build_constants_module(Runtime::RuntimeContext& rt) {
    auto& s = rt.shapes;
    auto shape_val = [&](uint64_t addr) {
        return Runtime::Value::from_pod(s.shape_meta, *reinterpret_cast<const Runtime::ShapeLayout*>(addr));
    };

    return build_struct(rt, {
        {"true", rt.make_bool(true)},
        {"false", rt.make_bool(false)},
        {"u64_zero", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 0ULL)},
        {"u64_one", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 1ULL)},
        {"u64_max", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, std::numeric_limits<uint64_t>::max())},
        {"unit_shape", shape_val(s.unit_shape)},
        {"raw1_shape", shape_val(s.raw1_shape)},
        {"raw4_shape", shape_val(s.raw4_shape)},
        {"raw8_shape", shape_val(s.raw8_shape)},
        {"symbol_shape", shape_val(s.symbol_shape)},
        {"boolean_shape", shape_val(s.boolean_shape)},
        {"pointer_shape", shape_val(s.pointer_shape)},
        {"field_entry_shape", shape_val(s.field_entry_shape)},
        {"field_map_shape", shape_val(s.field_map_shape)},
        {"shape_meta", shape_val(s.shape_meta)},
        {"prototype_shape", shape_val(s.prototype_shape)},
        {"handle_shape", shape_val(s.handle_shape)},
    });
}

Runtime::Value CapabilityHost::build_field_map_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"empty", native_proto(rt, "capabilities.field_map.empty", [](Evaluator& ev) {
            return Runtime::Value::from_pod(ev.rt.shapes.field_map_shape, Runtime::FieldMapArena::empty());
        })},
        {"is_empty", native_proto(rt, "capabilities.field_map.is_empty", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            return ev.rt.make_bool(Runtime::FieldMapArena::is_empty(fm));
        })},
        {"count", native_proto(rt, "capabilities.field_map.count", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, Runtime::FieldMapArena::count(fm));
        })},
        {"get", native_proto(rt, "capabilities.field_map.get", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            return ev.rt.make_pointer(Runtime::FieldMapArena::get(fm, sym));
        })},
        {"at", native_proto(rt, "capabilities.field_map.at", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            auto idx = static_cast<uint32_t>(read_field(ev, "index").as_pod<uint64_t>());
            return Runtime::Value::from_pod(ev.rt.shapes.field_entry_shape, Runtime::FieldMapArena::at(fm, idx));
        })},
        {"insert", native_proto(rt, "capabilities.field_map.insert", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            auto ptr = read_field(ev, "ptr").as_pod<Runtime::PointerLayout>();
            auto next = ev.rt.shapes.field_maps.insert(fm, sym, ptr);
            return Runtime::Value::from_pod(ev.rt.shapes.field_map_shape, next);
        })},
        {"ptr_shape", native_proto(rt, "capabilities.field_map.ptr_shape", [](Evaluator& ev) {
            auto p = read_field(ev, "ptr").as_pod<Runtime::PointerLayout>();
            if (p.shape_ptr == 0) {
                throw std::runtime_error("capabilities.field_map.ptr_shape: null shape_ptr");
            }
            const auto* sl = reinterpret_cast<const Runtime::ShapeLayout*>(p.shape_ptr);
            return Runtime::Value::from_pod(ev.rt.shapes.shape_meta, *sl);
        })},
    });
}

Runtime::Value CapabilityHost::build_symbols_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"byte_len", native_proto(rt, "capabilities.symbols.byte_len", [](Evaluator& ev) {
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            return Runtime::Value::from_pod<uint64_t>(ev.rt.shapes.raw8_shape, ev.rt.symbols.name(sym).size());
        })},
        {"byte_at", native_proto(rt, "capabilities.symbols.byte_at", [](Evaluator& ev) {
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            auto idx = read_field(ev, "index").as_pod<uint64_t>();
            std::string_view sv = ev.rt.symbols.name(sym);
            if (idx >= sv.size()) throw std::out_of_range("capabilities.symbols.byte_at: out of range");
            return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, static_cast<uint8_t>(sv[idx]));
        })},
        {"to_bytes", native_proto(rt, "capabilities.symbols.to_bytes", [](Evaluator& ev) {
            auto sym = read_field(ev, "sym").as_pod<Runtime::SymbolId>();
            std::string_view sv = ev.rt.symbols.name(sym);
            return ev.rt.make_bytes_ptr(sv.data(), sv.size());
        })},
        {"intern_bytes", native_proto(rt, "capabilities.symbols.intern_bytes", [](Evaluator& ev) {
            auto span = read_bytes_ptr(ev, "bytes");
            return ev.rt.make_symbol(ev.rt.symbols.intern(span));
        })},
    });
}

Runtime::Value CapabilityHost::build_intrinsics_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"add_i64", native_proto(rt, "capabilities.intrinsics.add_i64", [](Evaluator& ev) {
            return Runtime::Value::from_pod<int64_t>(
                ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() + read_field(ev, "b").as_pod<int64_t>());
        })},
        {"sub_i64", native_proto(rt, "capabilities.intrinsics.sub_i64", [](Evaluator& ev) {
            return Runtime::Value::from_pod<int64_t>(
                ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() - read_field(ev, "b").as_pod<int64_t>());
        })},
        {"mul_i64", native_proto(rt, "capabilities.intrinsics.mul_i64", [](Evaluator& ev) {
            return Runtime::Value::from_pod<int64_t>(
                ev.rt.shapes.raw8_shape, read_field(ev, "a").as_pod<int64_t>() * read_field(ev, "b").as_pod<int64_t>());
        })},
        {"lt_i64", native_proto(rt, "capabilities.intrinsics.lt_i64", [](Evaluator& ev) {
            return ev.rt.make_bool(read_field(ev, "a").as_pod<int64_t>() < read_field(ev, "b").as_pod<int64_t>());
        })},
        {"eq_i64", native_proto(rt, "capabilities.intrinsics.eq_i64", [](Evaluator& ev) {
            return ev.rt.make_bool(read_field(ev, "a").as_pod<int64_t>() == read_field(ev, "b").as_pod<int64_t>());
        })},
        {"eq_sym", native_proto(rt, "capabilities.intrinsics.eq_sym", [](Evaluator& ev) {
            return ev.rt.make_bool(
                read_field(ev, "a").as_pod<Runtime::SymbolId>() == read_field(ev, "b").as_pod<Runtime::SymbolId>());
        })},
        {"eq_shape", native_proto(rt, "capabilities.intrinsics.eq_shape", [](Evaluator& ev) {
            auto a = read_field(ev, "a").as_pod<Runtime::ShapeLayout>();
            auto b = read_field(ev, "b").as_pod<Runtime::ShapeLayout>();
            return ev.rt.make_bool(ev.canonical_shape_addr(a) == ev.canonical_shape_addr(b));
        })},
        {"bytes_at", native_proto(rt, "capabilities.intrinsics.bytes_at", [](Evaluator& ev) {
            auto span = read_bytes_ptr(ev, "bytes");
            auto idx = read_field(ev, "index").as_pod<uint64_t>();
            if (idx >= span.size()) throw std::out_of_range("capabilities.intrinsics.bytes_at: out of range");
            return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, span[idx]);
        })},
        {"bytes_slice", native_proto(rt, "capabilities.intrinsics.bytes_slice", [](Evaluator& ev) {
            auto span = read_bytes_ptr(ev, "bytes");
            auto start = read_field(ev, "start").as_pod<uint64_t>();
            auto len = read_field(ev, "len").as_pod<uint64_t>();
            if (start > span.size() || start + len > span.size()) {
                throw std::out_of_range("capabilities.intrinsics.bytes_slice: out of range");
            }
            return ev.rt.make_bytes_ptr(span.data() + start, len);
        })},
        {"u8_to_i64", native_proto(rt, "capabilities.intrinsics.u8_to_i64", [](Evaluator& ev) {
            int64_t v = static_cast<int64_t>(read_field(ev, "b").as_pod<uint8_t>());
            return Runtime::Value::from_pod<int64_t>(ev.rt.shapes.raw8_shape, v);
        })},
    });
}

} // namespace reverSilly::Lang::Eval
