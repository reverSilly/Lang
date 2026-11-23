#include "Capabilities.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace reverSilly::Lang::Eval::CapabilitiesDetail {

namespace {

uint8_t read_int_width(Evaluator& ev, std::string_view field_name, std::string_view op_name) {
    uint64_t w = read_field(ev, field_name).as_pod<uint64_t>();
    if (w != 1 && w != 2 && w != 4 && w != 8) {
        throw std::runtime_error(
            "capabilities.intrinsics." + std::string(op_name) + ": invalid integer width " + std::to_string(w));
    }
    return static_cast<uint8_t>(w);
}

uint8_t read_float_width(Evaluator& ev, std::string_view field_name, std::string_view op_name) {
    uint64_t w = read_field(ev, field_name).as_pod<uint64_t>();
    if (w != 4 && w != 8) {
        throw std::runtime_error(
            "capabilities.intrinsics." + std::string(op_name) + ": invalid float width " + std::to_string(w));
    }
    return static_cast<uint8_t>(w);
}

bool read_signed_flag(Evaluator& ev) {
    return read_field(ev, "signed").as_pod<uint8_t>() != 0;
}

Runtime::SymbolId read_sym_field(Evaluator& ev, std::string_view field_name) {
    Runtime::Value v = read_field(ev, field_name);
    if (v.shape_addr != ev.rt.shapes.symbol_shape) {
        throw std::runtime_error(
            "capabilities.intrinsics: field '" + std::string(field_name) + "' must be a Symbol");
    }
    return v.as_pod<Runtime::SymbolId>();
}

template <Runtime::IntBinKind Kind>
Runtime::Value make_int_binop_factory(Runtime::RuntimeContext& rt, std::string name) {
    return native_proto(rt, "capabilities.intrinsics." + name, [name](Evaluator& ev) {
        Runtime::IntBinOp op{
            Kind,
            read_int_width(ev, "width", name),
            read_sym_field(ev, "lhs"),
            read_sym_field(ev, "rhs"),
        };
        return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
    });
}

template <Runtime::IntBinKind SignedKind, Runtime::IntBinKind UnsignedKind>
Runtime::Value make_signed_int_binop_factory(Runtime::RuntimeContext& rt, std::string name) {
    return native_proto(rt, "capabilities.intrinsics." + name, [name](Evaluator& ev) {
        Runtime::IntBinOp op{
            read_signed_flag(ev) ? SignedKind : UnsignedKind,
            read_int_width(ev, "width", name),
            read_sym_field(ev, "lhs"),
            read_sym_field(ev, "rhs"),
        };
        return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
    });
}

template <Runtime::FloatBinKind Kind>
Runtime::Value make_float_binop_factory(Runtime::RuntimeContext& rt, std::string name) {
    return native_proto(rt, "capabilities.intrinsics." + name, [name](Evaluator& ev) {
        Runtime::FloatBinOp op{
            Kind,
            read_float_width(ev, "width", name),
            read_sym_field(ev, "lhs"),
            read_sym_field(ev, "rhs"),
        };
        return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
    });
}

template <Runtime::CmpKind SignedKind, Runtime::CmpKind UnsignedKind>
Runtime::Value make_int_cmp_factory(Runtime::RuntimeContext& rt, std::string name) {
    return native_proto(rt, "capabilities.intrinsics." + name, [name](Evaluator& ev) {
        Runtime::CmpOp op{
            read_signed_flag(ev) ? SignedKind : UnsignedKind,
            read_int_width(ev, "width", name),
            read_sym_field(ev, "lhs"),
            read_sym_field(ev, "rhs"),
        };
        return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
    });
}

template <Runtime::CmpKind Kind>
Runtime::Value make_float_cmp_factory(Runtime::RuntimeContext& rt, std::string name) {
    return native_proto(rt, "capabilities.intrinsics." + name, [name](Evaluator& ev) {
        Runtime::CmpOp op{
            Kind,
            read_float_width(ev, "width", name),
            read_sym_field(ev, "lhs"),
            read_sym_field(ev, "rhs"),
        };
        return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
    });
}

} // namespace

Runtime::Value build_constants_module(Runtime::RuntimeContext& rt) {
    auto& s = rt.shapes;
    auto shape_val = [&](uint64_t addr) {
        return Runtime::Value::from_pod(s.shape_meta, *reinterpret_cast<const Runtime::ShapeLayout*>(addr));
    };

    return build_struct(rt, {
        {"true", Runtime::Value::from_pod<uint8_t>(s.raw1_shape, 1)},
        {"false", Runtime::Value::from_pod<uint8_t>(s.raw1_shape, 0)},
        {"u64_zero", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 0ULL)},
        {"u64_one", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, 1ULL)},
        {"u64_max", Runtime::Value::from_pod<uint64_t>(s.raw8_shape, std::numeric_limits<uint64_t>::max())},
        {"unit_shape", shape_val(s.unit_shape)},
        {"raw1_shape", shape_val(s.raw1_shape)},
        {"raw2_shape", shape_val(s.raw2_shape)},
        {"raw4_shape", shape_val(s.raw4_shape)},
        {"raw8_shape", shape_val(s.raw8_shape)},
        {"symbol_shape", shape_val(s.symbol_shape)},
        {"pointer_shape", shape_val(s.pointer_shape)},
        {"field_entry_shape", shape_val(s.field_entry_shape)},
        {"field_map_shape", shape_val(s.field_map_shape)},
        {"shape_meta", shape_val(s.shape_meta)},
        {"prototype_shape", shape_val(s.prototype_shape)},
        {"handle_shape", shape_val(s.handle_shape)},
    });
}

Runtime::Value build_field_map_module(Runtime::RuntimeContext& rt) {
    return build_struct(rt, {
        {"empty", native_proto(rt, "capabilities.field_map.empty", [](Evaluator& ev) {
            return Runtime::Value::from_pod(ev.rt.shapes.field_map_shape, Runtime::FieldMapArena::empty());
        })},
        {"is_empty", native_proto(rt, "capabilities.field_map.is_empty", [](Evaluator& ev) {
            auto fm = read_field(ev, "map").as_pod<Runtime::FieldMapInline>();
            return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, Runtime::FieldMapArena::is_empty(fm) ? 1 : 0);
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

Runtime::Value build_symbols_module(Runtime::RuntimeContext& rt) {
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
            return ev.rt.make_raw_bytes(sv);
        })},
        {"intern_bytes", native_proto(rt, "capabilities.symbols.intern_bytes", [](Evaluator& ev) {
            auto raw = read_raw_bytes(ev, "bytes");
            return ev.rt.make_symbol(ev.rt.symbols.intern(raw));
        })},
    });
}

Runtime::Value build_intrinsics_module(Runtime::RuntimeContext& rt) {
    using IB = Runtime::IntBinKind;
    using IU = Runtime::IntUnaryKind;
    using FB = Runtime::FloatBinKind;
    using CK = Runtime::CastKind;
    using CM = Runtime::CmpKind;

    return build_struct(rt, {
        // Integer binary arithmetic -> bytes(width)
        {"add_int", make_int_binop_factory<IB::Add>(rt, "add_int")},
        {"sub_int", make_int_binop_factory<IB::Sub>(rt, "sub_int")},
        {"mul_int", make_int_binop_factory<IB::Mul>(rt, "mul_int")},
        {"div_int", make_signed_int_binop_factory<IB::SDiv, IB::UDiv>(rt, "div_int")},
        {"rem_int", make_signed_int_binop_factory<IB::SRem, IB::URem>(rt, "rem_int")},

        // Bitwise & shift -> bytes(width)
        {"bit_and", make_int_binop_factory<IB::BitAnd>(rt, "bit_and")},
        {"bit_or", make_int_binop_factory<IB::BitOr>(rt, "bit_or")},
        {"bit_xor", make_int_binop_factory<IB::BitXor>(rt, "bit_xor")},
        {"bit_not", native_proto(rt, "capabilities.intrinsics.bit_not", [](Evaluator& ev) {
            Runtime::IntUnaryOp op{IU::BitNot, read_int_width(ev, "width", "bit_not"), read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"shl", make_int_binop_factory<IB::Shl>(rt, "shl")},
        {"shr", make_signed_int_binop_factory<IB::AShr, IB::LShr>(rt, "shr")},

        // Integer width conversions -> bytes(to_width)
        {"ext_int", native_proto(rt, "capabilities.intrinsics.ext_int", [](Evaluator& ev) {
            bool is_signed = read_signed_flag(ev);
            uint8_t from_w = read_int_width(ev, "from", "ext_int");
            uint8_t to_w = read_int_width(ev, "to", "ext_int");
            if (from_w >= to_w) throw std::runtime_error("capabilities.intrinsics.ext_int: requires from < to");
            Runtime::CastOp op{is_signed ? CK::SExt : CK::ZExt, from_w, to_w, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"trunc_int", native_proto(rt, "capabilities.intrinsics.trunc_int", [](Evaluator& ev) {
            uint8_t from_w = read_int_width(ev, "from", "trunc_int");
            uint8_t to_w = read_int_width(ev, "to", "trunc_int");
            if (from_w <= to_w) throw std::runtime_error("capabilities.intrinsics.trunc_int: requires from > to");
            Runtime::CastOp op{CK::Trunc, from_w, to_w, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},

        // Float binary arithmetic -> bytes(width)
        {"add_float", make_float_binop_factory<FB::Add>(rt, "add_float")},
        {"sub_float", make_float_binop_factory<FB::Sub>(rt, "sub_float")},
        {"mul_float", make_float_binop_factory<FB::Mul>(rt, "mul_float")},
        {"div_float", make_float_binop_factory<FB::Div>(rt, "div_float")},

        // Float / integer conversions -> bytes(to_width)
        {"ext_float", native_proto(rt, "capabilities.intrinsics.ext_float", [](Evaluator& ev) {
            Runtime::CastOp op{CK::FPExt, 4, 8, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"trunc_float", native_proto(rt, "capabilities.intrinsics.trunc_float", [](Evaluator& ev) {
            Runtime::CastOp op{CK::FPTrunc, 8, 4, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"int_to_float", native_proto(rt, "capabilities.intrinsics.int_to_float", [](Evaluator& ev) {
            bool is_signed = read_signed_flag(ev);
            uint8_t from_w = read_int_width(ev, "from", "int_to_float");
            uint8_t to_w = read_float_width(ev, "to", "int_to_float");
            Runtime::CastOp op{is_signed ? CK::SIToFP : CK::UIToFP, from_w, to_w, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"float_to_int", native_proto(rt, "capabilities.intrinsics.float_to_int", [](Evaluator& ev) {
            bool is_signed = read_signed_flag(ev);
            uint8_t from_w = read_float_width(ev, "from", "float_to_int");
            uint8_t to_w = read_int_width(ev, "to", "float_to_int");
            Runtime::CastOp op{is_signed ? CK::FPToSI : CK::FPToUI, from_w, to_w, read_sym_field(ev, "src")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},

        // Comparisons -> bytes(1, 1) (0x01 / 0x00)
        {"eq_bytes", native_proto(rt, "capabilities.intrinsics.eq_bytes", [](Evaluator& ev) {
            uint64_t w = read_field(ev, "width").as_pod<uint64_t>();
            if (w == 0 || w > 255) {
                throw std::runtime_error("capabilities.intrinsics.eq_bytes: width must be in 1..255");
            }
            Runtime::CmpOp op{CM::EqBytes, static_cast<uint8_t>(w), read_sym_field(ev, "lhs"), read_sym_field(ev, "rhs")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"lt_int", make_int_cmp_factory<CM::SLt, CM::ULt>(rt, "lt_int")},
        {"le_int", make_int_cmp_factory<CM::SLe, CM::ULe>(rt, "le_int")},
        {"eq_float", make_float_cmp_factory<CM::FEq>(rt, "eq_float")},
        {"lt_float", make_float_cmp_factory<CM::FLt>(rt, "lt_float")},
        {"le_float", make_float_cmp_factory<CM::FLe>(rt, "le_float")},

        // Structural Shape equality (IntrinsicProto factory) & RawBytes indexing
        {"eq_shape", native_proto(rt, "capabilities.intrinsics.eq_shape", [](Evaluator& ev) {
            Runtime::CmpOp op{CM::EqShape, 0, read_sym_field(ev, "lhs"), read_sym_field(ev, "rhs")};
            return ev.rt.make_proto(ev.rt.prototypes.create_intrinsic(op));
        })},
        {"bytes_at", native_proto(rt, "capabilities.intrinsics.bytes_at", [](Evaluator& ev) {
            auto raw = read_raw_bytes(ev, "bytes");
            auto idx = read_field(ev, "index").as_pod<uint64_t>();
            if (idx >= raw.size()) throw std::out_of_range("capabilities.intrinsics.bytes_at: out of range");
            return Runtime::Value::from_pod<uint8_t>(ev.rt.shapes.raw1_shape, raw[idx]);
        })},
        {"bytes_slice", native_proto(rt, "capabilities.intrinsics.bytes_slice", [](Evaluator& ev) {
            auto raw = read_raw_bytes(ev, "bytes");
            auto start = read_field(ev, "start").as_pod<uint64_t>();
            auto len = read_field(ev, "len").as_pod<uint64_t>();
            if (start > raw.size() || start + len > raw.size()) {
                throw std::out_of_range("capabilities.intrinsics.bytes_slice: out of range");
            }
            return ev.rt.make_raw_bytes(raw.data() + start, len);
        })},
    });
}

} // namespace reverSilly::Lang::Eval::CapabilitiesDetail
