#include "CliArgs.h"

#include <string>

namespace reverSilly::Lang::Eval {

void install_cli_args(Runtime::RuntimeContext& rt, const std::vector<std::string_view>& args) {
    Runtime::ConstructingFrame tmp;
    Runtime::Value count_val =
        Runtime::Value::from_pod(rt.shapes.raw8_shape, static_cast<uint64_t>(args.size()));
    Runtime::FrameSlot* count_slot = tmp.allocate_val(count_val, false);
    tmp.bind(rt.symbols.intern("count"), count_slot->pointer());

    for (size_t i = 0; i < args.size(); ++i) {
        Runtime::Value arg_val = rt.make_raw_bytes(args[i]);
        Runtime::FrameSlot* arg_slot = tmp.allocate_val(arg_val, false);
        tmp.bind(rt.symbols.intern(std::to_string(i)), arg_slot->pointer());
    }

    Runtime::Value args_obj = tmp.close_and_flatten(rt.shapes);
    Runtime::ConstructingFrame* root = rt.current_frame();
    Runtime::FrameSlot* slot = root->allocate_val(args_obj, false);
    root->bind(rt.symbols.intern("args"), slot->pointer());
}

} // namespace reverSilly::Lang::Eval
