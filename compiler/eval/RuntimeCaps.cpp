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

Runtime::Value runtime_intrinsic(Runtime::RuntimeContext& rt, Runtime::RuntimeIntrinsicKind kind) {
    return rt.make_proto(rt.prototypes.create_intrinsic(Runtime::RuntimeOp{kind}));
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
        return rt.make_pointer(Runtime::PointerLayout{addr, s.shape_meta, 0});
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
    using RK = Runtime::RuntimeIntrinsicKind;
    return build_struct(rt, {
        {"empty", runtime_intrinsic(rt, RK::FieldMapEmpty)},
        {"is_empty", runtime_intrinsic(rt, RK::FieldMapIsEmpty)},
        {"count", runtime_intrinsic(rt, RK::FieldMapCount)},
        {"get", runtime_intrinsic(rt, RK::FieldMapGet)},
        {"at", runtime_intrinsic(rt, RK::FieldMapAt)},
        {"insert", runtime_intrinsic(rt, RK::FieldMapInsert)},
        {"ptr_shape", runtime_intrinsic(rt, RK::FieldMapPtrShape)},
    });
}

Runtime::Value build_shapes_module(Runtime::RuntimeContext& rt) {
    using RK = Runtime::RuntimeIntrinsicKind;
    return build_struct(rt, {
        {"intern_shape", runtime_intrinsic(rt, RK::InternShape)},
    });
}

Runtime::Value build_symbols_module(Runtime::RuntimeContext& rt) {
    using RK = Runtime::RuntimeIntrinsicKind;
    return build_struct(rt, {
        {"byte_len", runtime_intrinsic(rt, RK::SymbolByteLen)},
        {"byte_at", runtime_intrinsic(rt, RK::SymbolByteAt)},
        {"to_bytes", runtime_intrinsic(rt, RK::SymbolToBytes)},
        {"intern_bytes", runtime_intrinsic(rt, RK::SymbolInternBytes)},
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

        // RawBytes indexing, slicing & concatenation (stageable IntrinsicProto)
        {"bytes_at", runtime_intrinsic(rt, Runtime::RuntimeIntrinsicKind::BytesAt)},
        {"bytes_slice", runtime_intrinsic(rt, Runtime::RuntimeIntrinsicKind::BytesSlice)},
        {"bytes_concat", runtime_intrinsic(rt, Runtime::RuntimeIntrinsicKind::BytesConcat)},
    });
}

} // namespace reverSilly::Lang::Eval::CapabilitiesDetail
