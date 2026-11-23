#include "eval/Capabilities.h"
#include "eval/Evaluator.h"
#include "parser/Layer0.h"
#include "runtime/Runtime.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: lang <entry.lang> [args...]\n";
        return 1;
    }

    const std::string path = argv[1];
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        std::cerr << "lang: failed to open '" << path << "'\n";
        return 1;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    const std::string source = oss.str();

    std::vector<std::string_view> cli_args;
    cli_args.reserve(static_cast<size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
        cli_args.emplace_back(argv[i]);
    }

    try {
        reverSilly::Lang::Runtime::RuntimeContext rt;
        reverSilly::Lang::Eval::install_all_capabilities(rt);
        reverSilly::Lang::Eval::install_cli_args(rt, cli_args);
        reverSilly::Lang::Eval::Evaluator ev(rt);
        reverSilly::Lang::Parser::Frontend fe;

        auto ast = fe.parse(source);
        (void)ev.run(ast);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "lang error: " << ex.what() << "\n";
        return 1;
    }
}
