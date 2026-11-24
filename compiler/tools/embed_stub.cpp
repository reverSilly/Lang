#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: embed_stub <input_binary> <output_cpp>\n";
        return 1;
    }

    std::ifstream ifs(argv[1], std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::cerr << "embed_stub: failed to open '" << argv[1] << "'\n";
        return 1;
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        std::cerr << "embed_stub: input binary is empty\n";
        return 1;
    }
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(data.data()), sz)) {
        std::cerr << "embed_stub: failed to read '" << argv[1] << "'\n";
        return 1;
    }

    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out) {
        std::cerr << "embed_stub: failed to open output '" << argv[2] << "'\n";
        return 1;
    }

    std::fputs("#include \"eval/EmbeddedStub.h\"\n\n", out);
    std::fputs("namespace reverSilly::Lang::Eval {\n", out);
    std::fputs("namespace {\n", out);
    std::fputs("alignas(16) constexpr uint8_t kStubBytes[] = {\n", out);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string line;
    line.reserve(128);
    for (size_t i = 0; i < data.size(); i += 16) {
        line.clear();
        line.append("    ");
        const size_t end = (i + 16 < data.size()) ? (i + 16) : data.size();
        for (size_t j = i; j < end; ++j) {
            const uint8_t b = data[j];
            line.push_back('0');
            line.push_back('x');
            line.push_back(kHex[b >> 4]);
            line.push_back(kHex[b & 0xFu]);
            line.push_back(',');
            if (j + 1 < end) line.push_back(' ');
        }
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), out);
    }

    std::fputs("};\n", out);
    std::fputs("} // namespace\n\n", out);
    std::fputs("std::span<const uint8_t> embedded_lang_stub() {\n", out);
    std::fputs("    return {kStubBytes, sizeof(kStubBytes)};\n", out);
    std::fputs("}\n", out);
    std::fputs("} // namespace reverSilly::Lang::Eval\n", out);

    if (std::fclose(out) != 0) {
        std::cerr << "embed_stub: failed to close output '" << argv[2] << "'\n";
        return 1;
    }
    return 0;
}
