#pragma once

#include "../ast/Ast.h"
#include "../parser/Layer0.h"
#include "Evaluator.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reverSilly::Lang::Eval {

class CapabilityHost {
private:
    enum class HandleTag : uint8_t {
        Ast = 1,
        Peg = 2,
        Frontend = 3,
        Cst = 4,
    };

    std::vector<Ast::ExprPtr> ast_pool_;
    std::vector<Peg::PegExprPtr> peg_pool_;
    std::vector<std::unique_ptr<Parser::Frontend>> frontend_pool_;
    std::vector<const Peg::Evaluator<Ast::ExprPtr>::Context*> cst_pool_;
    std::vector<std::unique_ptr<std::string>> byte_buffers_;

    static uint64_t encode_handle(HandleTag tag, size_t idx) noexcept;
    static size_t decode_handle(uint64_t handle, HandleTag expected, size_t pool_size, const char* what);
    static Runtime::Value read_field(Evaluator& ev, std::string_view name);
    static std::span<const uint8_t> read_bytes_ptr(Evaluator& ev, std::string_view name);
    static std::vector<Runtime::Value> read_positional(Evaluator& ev);
    static Runtime::Value build_struct(
        Runtime::RuntimeContext& rt,
        const std::vector<std::pair<std::string_view, Runtime::Value>>& fields);

    Runtime::Value store_bytes_ptr(Runtime::RuntimeContext& rt, std::string data);
    uint64_t store_ast(Ast::ExprPtr expr);
    Ast::ExprPtr load_ast(uint64_t handle) const;
    uint64_t store_peg(Peg::PegExprPtr expr);
    Peg::PegExprPtr load_peg(uint64_t handle) const;
    uint64_t store_frontend(std::unique_ptr<Parser::Frontend> fe);
    Parser::Frontend* load_frontend(uint64_t handle) const;
    const Peg::Evaluator<Ast::ExprPtr>::Context* load_cst(uint64_t handle) const;
    Runtime::Value native_proto(Runtime::RuntimeContext& rt, std::string name, Runtime::NativeFn fn);

public:
    void install(Runtime::RuntimeContext& rt);
};

} // namespace reverSilly::Lang::Eval
