#include "decrypton/app.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr std::string_view kVersion = "0.4.0";

void print_suite_help() {
    std::printf(
        "Decrypton %.*s - Windows PE capture and offline analysis suite\n\n"
        "Usage:\n"
        "  decrypton capture --process <name> [capture options]\n"
        "  decrypton inspect <file> [--json <report>]\n  decrypton analyze <file> [--json <report>]\n"
        "  decrypton verify <file> [--strict] [--json <report>]\n"
        "  decrypton diff <original> <candidate> [--json <report>] [--block <bytes>]\n"
        "  decrypton transform <input> --algorithm <name> --output <file> [options]\n\n"
        "Backward compatibility:\n"
        "  decrypton --process <name> --output <file>\n"
        "  decrypton <process.exe> [limit-percent]\n\n"
        "Transform algorithms:\n"
        "  xor             Requires --key-hex\n"
        "  aes-256-cbc     Requires --mode encrypt|decrypt, --key-hex and --iv-hex\n\n"
        "Run 'decrypton <command> --help' for command-specific help.\n",
        static_cast<int>(kVersion.size()),
        kVersion.data());
}

bool equals(const char* value, std::string_view expected) {
    return value != nullptr && expected == value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        print_suite_help();
        return 0;
    }

    if (equals(argv[1], "--help") || equals(argv[1], "-h") ||
        equals(argv[1], "help")) {
        print_suite_help();
        return 0;
    }
    if (equals(argv[1], "--version") || equals(argv[1], "version")) {
        std::printf("decrypton %.*s\n", static_cast<int>(kVersion.size()), kVersion.data());
        return 0;
    }

    if (equals(argv[1], "capture")) {
        return run_capture(argc - 1, argv + 1);
    }
    if (equals(argv[1], "inspect") || equals(argv[1], "analyze")) {
        return run_inspect(argc - 1, argv + 1);
    }
    if (equals(argv[1], "verify")) {
        return run_verify(argc - 1, argv + 1);
    }
    if (equals(argv[1], "diff")) {
        return run_diff(argc - 1, argv + 1);
    }
    if (equals(argv[1], "transform")) {
        return run_transform(argc - 1, argv + 1);
    }

    // Preserve the v0.3 command line for existing scripts.
    return run_capture(argc, argv);
}
