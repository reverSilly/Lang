#include "ast/AstCodec.h"
#include "ast/PackFormat.h"
#include "eval/CliArgs.h"
#include "eval/Evaluator.h"
#include "runtime/Runtime.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::filesystem::path get_self_executable_path([[maybe_unused]] const char* argv0) {
#if defined(__linux__)
    return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        throw std::runtime_error("failed to resolve executable path via _NSGetExecutablePath");
    }
    return std::filesystem::path(buf.c_str());
#elif defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        throw std::runtime_error("failed to resolve executable path via GetModuleFileNameW");
    }
    buf.resize(len);
    return std::filesystem::path(buf);
#else
    return std::filesystem::path(argv0 ? argv0 : "");
#endif
}

reverSilly::Lang::Ast::ExprPtr load_packed_ast(const std::filesystem::path& exe_path) {
    using namespace reverSilly::Lang::Ast;

    std::ifstream ifs(exe_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("failed to open executable '" + exe_path.string() + "'");
    }

    const std::streamoff total_size = ifs.tellg();
    if (total_size < static_cast<std::streamoff>(kPackTrailerSize)) {
        throw std::runtime_error("executable does not contain a packed Lang AST trailer");
    }

    std::array<uint8_t, kPackTrailerSize> trailer{};
    ifs.seekg(total_size - static_cast<std::streamoff>(kPackTrailerSize), std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(trailer.data()), static_cast<std::streamsize>(trailer.size()))) {
        throw std::runtime_error("failed to read packed Lang AST trailer");
    }

    if (std::memcmp(trailer.data() + 8, kPackTrailerMagic.data(), kPackTrailerMagic.size()) != 0) {
        throw std::runtime_error("missing packed Lang AST trailer (magic mismatch)");
    }

    const uint64_t payload_size = decode_u64_le(trailer.data());
    const uint64_t max_payload = static_cast<uint64_t>(total_size) - kPackTrailerSize;
    if (payload_size == 0 || payload_size > max_payload) {
        throw std::runtime_error("corrupted packed Lang AST payload size");
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
    ifs.seekg(total_size - static_cast<std::streamoff>(kPackTrailerSize + payload_size), std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
        throw std::runtime_error("failed to read packed Lang AST payload");
    }

    return deserialize(payload);
}

int extract_exit_code(
    reverSilly::Lang::Runtime::RuntimeContext& rt,
    const reverSilly::Lang::Runtime::Value& val) {
    auto low_byte = [&](uint64_t shape_addr, size_t offset) -> int {
        if (shape_addr == rt.shapes.raw1_shape && offset + 1 <= val.bytes.size()) {
            uint8_t v = 0; std::memcpy(&v, val.bytes.data() + offset, 1);
            return static_cast<int>(v);
        }
        if (shape_addr == rt.shapes.raw2_shape && offset + 2 <= val.bytes.size()) {
            uint16_t v = 0; std::memcpy(&v, val.bytes.data() + offset, 2);
            return static_cast<int>(v & 0xFFu);
        }
        if (shape_addr == rt.shapes.raw4_shape && offset + 4 <= val.bytes.size()) {
            uint32_t v = 0; std::memcpy(&v, val.bytes.data() + offset, 4);
            return static_cast<int>(v & 0xFFu);
        }
        if (shape_addr == rt.shapes.raw8_shape && offset + 8 <= val.bytes.size()) {
            uint64_t v = 0; std::memcpy(&v, val.bytes.data() + offset, 8);
            return static_cast<int>(v & 0xFFu);
        }
        return -1;
    };
    if (int r = low_byte(val.shape_addr, 0); r >= 0) return r;
    if (const auto* sl = val.shape()) {
        const auto raw_sym = rt.symbols.intern("__raw");
        const auto rel = reverSilly::Lang::Runtime::FieldMapArena::get(sl->fields, raw_sym);
        if (!rel.is_null()) {
            if (int r = low_byte(rel.shape_ptr, static_cast<size_t>(rel.addr)); r >= 0) return r;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto self_path = get_self_executable_path(argc > 0 ? argv[0] : nullptr);
        const auto ast = load_packed_ast(self_path);

        std::vector<std::string_view> cli_args;
        cli_args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            cli_args.emplace_back(argv[i]);
        }

        reverSilly::Lang::Runtime::RuntimeContext rt;
        reverSilly::Lang::Eval::install_cli_args(rt, cli_args);
        reverSilly::Lang::Eval::Evaluator ev(rt);
        const reverSilly::Lang::Runtime::Value res = ev.run(ast);
        return extract_exit_code(rt, res);
    } catch (const std::exception& ex) {
        std::cerr << "lang runtime error: " << ex.what() << "\n";
        return 1;
    }
}
