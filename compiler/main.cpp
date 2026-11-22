#include "eval/Capabilities.h"
#include "eval/Evaluator.h"
#include "parser/Layer0.h"
#include "runtime/Runtime.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: lang <entry.lang>\n";
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

    try {
        reverSilly::Lang::Runtime::RuntimeContext rt;
        reverSilly::Lang::Eval::CapabilityHost caps;
        caps.install(rt);
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
