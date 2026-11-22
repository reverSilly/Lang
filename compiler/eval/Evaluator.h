#pragma once

#include "../ast/Ast.h"
#include "../runtime/Runtime.h"

#include <coroutine>
#include <cstdint>
#include <exception>
#include <utility>

namespace reverSilly::Lang::Eval {

struct BreakSignal {
    Runtime::Value value;
};

struct EvalTask {
    struct promise_type {
        std::coroutine_handle<> continuation = std::noop_coroutine();
        Runtime::Value result{};
        std::exception_ptr exception = nullptr;

        EvalTask get_return_object() noexcept {
            return EvalTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(Runtime::Value v) noexcept { result = std::move(v); }
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;
    Handle handle{};

    explicit EvalTask(Handle h) noexcept : handle(h) {}
    EvalTask(EvalTask&& o) noexcept : handle(std::exchange(o.handle, {})) {}
    EvalTask(const EvalTask&) = delete;
    EvalTask& operator=(const EvalTask&) = delete;

    ~EvalTask() {
        if (handle) handle.destroy();
    }

    struct Awaiter {
        Handle child;
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            child.promise().continuation = caller;
            return child;
        }
        Runtime::Value await_resume() {
            if (child.promise().exception) {
                std::rethrow_exception(child.promise().exception);
            }
            return std::move(child.promise().result);
        }
    };

    Awaiter operator co_await() && noexcept { return Awaiter{handle}; }
};

struct CoroEntry {
    struct promise_type {
        std::coroutine_handle<> next_on_finish = std::noop_coroutine();
        Runtime::Value result{};
        std::exception_ptr exception = nullptr;

        CoroEntry get_return_object() noexcept {
            return CoroEntry{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().next_on_finish;
            }
            void await_resume() noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(Runtime::Value v) noexcept { result = std::move(v); }
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;
    Handle handle{};

    explicit CoroEntry(Handle h) noexcept : handle(h) {}
    CoroEntry(CoroEntry&& o) noexcept : handle(std::exchange(o.handle, {})) {}
    CoroEntry(const CoroEntry&) = delete;
    CoroEntry& operator=(const CoroEntry&) = delete;

    Handle release() noexcept { return std::exchange(handle, {}); }

    ~CoroEntry() {
        if (handle) handle.destroy();
    }
};

// Core Evaluator: 4 AST Nodes + 22 Symmetric Core Builtins + C++20 Symmetric Coroutines
class Evaluator {
public:
    Runtime::RuntimeContext& rt;

    explicit Evaluator(Runtime::RuntimeContext& runtime) : rt(runtime) {}

    Runtime::Value run(const Ast::ExprPtr& expr);
    Runtime::Value eval_proto_sync(uint64_t proto_addr);
    uint64_t canonical_shape_addr(const Runtime::ShapeLayout& sl);

private:
    struct ResumeAwaiter {
        Evaluator& ev;
        Runtime::Coroutine* target;
        const Runtime::Value* payload;

        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> current_h);
        Runtime::Value await_resume();
    };

    CoroEntry run_main_entry(Ast::ExprPtr expr);
    CoroEntry run_proto_entry(uint64_t proto_addr);
    CoroEntry run_child_coro(Runtime::Coroutine* self, uint64_t proto_addr);
    EvalTask eval_proto(uint64_t proto_addr);
    EvalTask eval_expr(const Ast::ExprPtr& expr);
    EvalTask eval_builtin(const Ast::BuiltinExpr& b);
};

} // namespace reverSilly::Lang::Eval
