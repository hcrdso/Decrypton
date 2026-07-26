#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <DbgHelp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "Dbghelp.lib")
#endif

namespace {

constexpr std::string_view kVersion = "0.3.0";
constexpr SIZE_T kPageSize = 0x1000;


class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

namespace text {

std::wstring to_wide(std::string_view input) {
    if (input.empty()) {
        return {};
    }

    const int input_size = static_cast<int>((std::min)(
        input.size(), static_cast<size_t>((std::numeric_limits<int>::max)())));
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring output(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            input.data(),
            input_size,
            output.data(),
            required) != required) {
        return {};
    }
    return output;
}

std::string to_utf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }

    const int input_size = static_cast<int>((std::min)(
        input.size(), static_cast<size_t>((std::numeric_limits<int>::max)())));
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, input.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string output(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            input.data(),
            input_size,
            output.data(),
            required,
            nullptr,
            nullptr) != required) {
        return {};
    }
    return output;
}

std::string win32_error(DWORD code = GetLastError()) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);

    if (length == 0 || message == nullptr) {
        return "Win32 error " + std::to_string(code);
    }

    std::wstring_view view(message, length);
    while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n' ||
                              view.back() == L' ')) {
        view.remove_suffix(1);
    }

    std::string result = to_utf8(view);
    LocalFree(message);
    return result.empty() ? "Win32 error " + std::to_string(code) : result;
}

bool iequals(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return _wcsnicmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

}  // namespace text

namespace console {

constexpr const char* kRed = "\033[31m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kReset = "\033[0m";

void initialize() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr) {
        return;
    }

    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void banner() {
    std::printf(
        "%s%s"
        "________                                      __                 \n"
        "\\______ \\   ____   ___________ ___.__._______/  |_  ____   ____  \n"
        " |    |  \\_/ __ \\_/ ___\\_  __ <   |  |\\____ \\   __\\/  _ \\ /    \\ \n"
        " |    `   \\  ___/\\  \\___|  | \\/\\___  ||  |_> >  | (  <_> )   |  \\ \n"
        "/_______  /\\___  >\\___  >__|   / ____||   __/|__|  \\____/|___|  /\n"
        "        \\/     \\/     \\/       \\/     |__|                    \\/ \n"
        "%s",
        kBold,
        kCyan,
        kReset);
    std::printf("decrypton %.*s | hcrdso\n\n", static_cast<int>(kVersion.size()), kVersion.data());
}

template <typename... Args>
void print_message(const char* prefix, const char* color, const char* format, Args... args) {
    std::printf("%s%s%s%s ", kBold, color, prefix, kReset);
    if constexpr (sizeof...(Args) == 0) {
        std::fputs(format, stdout);
    } else {
        std::printf(format, args...);
    }
}

template <typename... Args>
void info(const char* format, Args... args) {
    print_message("[i]", kCyan, format, args...);
}

template <typename... Args>
void success(const char* format, Args... args) {
    print_message("[+]", kGreen, format, args...);
    std::printf("\n");
}

template <typename... Args>
void warning(const char* format, Args... args) {
    print_message("[!]", kYellow, format, args...);
    std::printf("\n");
}

template <typename... Args>
void failure(const char* format, Args... args) {
    print_message("[-]", kRed, format, args...);
    std::printf("\n");
}

void progress(
    std::string_view section,
    size_t current,
    size_t total,
    size_t bytes_read,
    size_t partial_pages,
    size_t failures) {
    const double percentage = total == 0
        ? 100.0
        : 100.0 * static_cast<double>(current) / static_cast<double>(total);
    std::printf(
        "\r  %-8.*s [%5zu/%5zu] %6.2f%%  read=%zu  partial=%zu  failed=%zu",
        static_cast<int>((std::min)(section.size(), size_t{8})),
        section.data(),
        current,
        total,
        percentage,
        bytes_read,
        partial_pages,
        failures);
    std::fflush(stdout);
}

}  // namespace console

struct Options {
    std::string process_name;
    std::optional<DWORD> pid;
    std::string module_name;
    std::filesystem::path output;
    std::filesystem::path report;
    std::filesystem::path minidump;
    double code_limit = 1.0;
    double minimum_code_coverage = 0.80;
    bool dump_data = true;
    bool rebuild_imports = true;
    bool aggressive_import_scan = false;
    bool force_import_rebuild = false;
    bool list_modules = false;
    bool write_report = true;
    bool show_help = false;
    bool show_version = false;
};

void print_usage() {
    std::printf(
        "Usage:\n"
        "  decrypton.exe --process <name> [options]\n"
        "  decrypton.exe --pid <id> [options]\n"
        "  decrypton.exe [process.exe] [limit-percent]\n\n"
        "Options:\n"
        "  -p, --process <name>       Target process name\n"
        "      --pid <id>             Target process ID\n"
        "  -m, --module <name>        Dump a loaded module instead of the main module\n"
        "      --list-modules         List loaded modules and exit\n"
        "  -o, --output <path>        Output PE path\n"
        "  -l, --limit <1-100>        Percentage of each executable section to copy\n"
        "      --min-coverage <1-100> Minimum code coverage before import rebuilding\n"
        "      --no-data              Keep non-executable sections from the disk image\n"
        "      --no-imports           Preserve original import structures\n"
        "      --aggressive-imports   Broaden resolved-import scanning\n"
        "      --force-imports        Keep a rebuilt import table even with weak evidence\n"
        "      --report <path>        Write a JSON coverage report\n"
        "      --no-report            Do not write a JSON report\n"
        "      --minidump <path>      Also create a standard Windows minidump\n"
        "      --version              Show the program version\n"
        "  -h, --help                 Show this help\n\n"
        "Decrypton reads only memory that Windows reports as accessible. It does not\n"
        "change remote page protections or invoke anti-tamper-specific mechanisms.\n");
}

bool parse_u32(std::string_view value, DWORD& output) {
    if (value.empty()) {
        return false;
    }

    uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0 || parsed > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }

    output = static_cast<DWORD>(parsed);
    return true;
}

bool parse_percent(std::string_view value, double& output) {
    if (value.empty()) {
        return false;
    }

    std::string owned(value);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (errno != 0 || end == owned.c_str() || *end != '\0' ||
        !std::isfinite(parsed) || parsed <= 0.0 || parsed > 100.0) {
        return false;
    }

    output = parsed / 100.0;
    return true;
}

std::optional<Options> parse_options(int argc, char** argv, std::string& error) {
    Options options;
    std::vector<std::string_view> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);

        auto require_value = [&](std::string_view option) -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                error = "missing value for " + std::string(option);
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };

        if (argument == "-h" || argument == "--help") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else if (argument == "-p" || argument == "--process") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            options.process_name = std::string(*value);
        } else if (argument == "--pid") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            DWORD pid = 0;
            if (!parse_u32(*value, pid)) {
                error = "invalid PID: " + std::string(*value);
                return std::nullopt;
            }
            options.pid = pid;
        } else if (argument == "-m" || argument == "--module") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            options.module_name = std::string(*value);
        } else if (argument == "--list-modules") {
            options.list_modules = true;
        } else if (argument == "-o" || argument == "--output") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            options.output = text::to_wide(*value);
        } else if (argument == "-l" || argument == "--limit") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            if (!parse_percent(*value, options.code_limit)) {
                error = "invalid limit; expected a value from 1 to 100";
                return std::nullopt;
            }
        } else if (argument == "--min-coverage") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            if (!parse_percent(*value, options.minimum_code_coverage)) {
                error = "invalid minimum coverage; expected a value from 1 to 100";
                return std::nullopt;
            }
        } else if (argument == "--no-data") {
            options.dump_data = false;
        } else if (argument == "--no-imports") {
            options.rebuild_imports = false;
        } else if (argument == "--aggressive-imports") {
            options.aggressive_import_scan = true;
        } else if (argument == "--force-imports") {
            options.force_import_rebuild = true;
        } else if (argument == "--report") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            options.report = text::to_wide(*value);
            options.write_report = true;
        } else if (argument == "--no-report") {
            options.write_report = false;
        } else if (argument == "--minidump") {
            const auto value = require_value(argument);
            if (!value) {
                return std::nullopt;
            }
            options.minidump = text::to_wide(*value);
        } else if (!argument.empty() && argument.front() == '-') {
            error = "unknown option: " + std::string(argument);
            return std::nullopt;
        } else {
            positional.push_back(argument);
        }
    }

    if (!positional.empty()) {
        options.process_name = std::string(positional[0]);
    }
    if (positional.size() >= 2 && !parse_percent(positional[1], options.code_limit)) {
        error = "invalid positional limit; expected a value from 1 to 100";
        return std::nullopt;
    }
    if (positional.size() > 2) {
        error = "too many positional arguments";
        return std::nullopt;
    }

    if (!options.show_help &&
        !options.show_version &&
        options.process_name.empty() &&
        !options.pid) {
        error = "a process name or PID is required";
        return std::nullopt;
    }

    return options;
}

namespace process {

struct ModuleInfo {
    PVOID base = nullptr;
    DWORD size = 0;
    std::wstring name;
    std::filesystem::path path;
};

DWORD find(std::wstring_view name) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return 0;
    }

    do {
        if (text::iequals(entry.szExeFile, name)) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));

    return 0;
}

std::optional<ModuleInfo> main_module(DWORD pid, std::wstring_view preferred_name) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return std::nullopt;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return std::nullopt;
    }

    std::optional<ModuleInfo> first;
    do {
        ModuleInfo current{
            entry.modBaseAddr,
            entry.modBaseSize,
            entry.szModule,
            entry.szExePath,
        };

        if (!first) {
            first = current;
        }
        if (!preferred_name.empty() && text::iequals(entry.szModule, preferred_name)) {
            return current;
        }
    } while (Module32NextW(snapshot.get(), &entry));

    return preferred_name.empty() ? first : std::nullopt;
}

std::vector<MODULEENTRY32W> modules(DWORD pid) {
    std::vector<MODULEENTRY32W> output;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return output;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return output;
    }

    do {
        output.push_back(entry);
    } while (Module32NextW(snapshot.get(), &entry));
    return output;
}

}  // namespace process

namespace memory {

struct ReadSpan {
    SIZE_T offset = 0;
    SIZE_T size = 0;
};

std::optional<MEMORY_BASIC_INFORMATION> query(
    HANDLE process,
    const void* address) {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQueryEx(
            process,
            address,
            &information,
            sizeof(information)) != sizeof(information)) {
        return std::nullopt;
    }
    return information;
}

bool is_readable(const MEMORY_BASIC_INFORMATION& information) {
    return information.State == MEM_COMMIT &&
        information.Protect != 0 &&
        (information.Protect & PAGE_GUARD) == 0 &&
        (information.Protect & PAGE_NOACCESS) == 0;
}

SIZE_T read(HANDLE process, const void* address, void* output, SIZE_T size) {
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, address, output, size, &bytes_read);
    return bytes_read;
}

SIZE_T read_resilient_impl(
    HANDLE process,
    const BYTE* address,
    BYTE* output,
    SIZE_T size,
    SIZE_T base_offset,
    std::vector<ReadSpan>& spans,
    unsigned depth) {
    if (size == 0) {
        return 0;
    }

    SIZE_T count = 0;
    const BOOL success = ReadProcessMemory(
        process,
        address,
        output,
        size,
        &count);

    if (count > 0) {
        spans.push_back({base_offset, count});
        if (count < size) {
            return count + read_resilient_impl(
                process,
                address + count,
                output + count,
                size - count,
                base_offset + count,
                spans,
                depth + 1);
        }
        return count;
    }

    constexpr SIZE_T kMinimumFragment = 64;
    constexpr unsigned kMaximumDepth = 12;
    if (success || size <= kMinimumFragment || depth >= kMaximumDepth) {
        return 0;
    }

    SIZE_T left_size = size / 2;
    left_size -= left_size % kMinimumFragment;
    if (left_size == 0 || left_size >= size) {
        return 0;
    }

    const SIZE_T right_size = size - left_size;
    return read_resilient_impl(
               process,
               address,
               output,
               left_size,
               base_offset,
               spans,
               depth + 1) +
        read_resilient_impl(
               process,
               address + left_size,
               output + left_size,
               right_size,
               base_offset + left_size,
               spans,
               depth + 1);
}

SIZE_T read_resilient(
    HANDLE process,
    const void* address,
    void* output,
    SIZE_T size,
    std::vector<ReadSpan>& spans) {
    spans.clear();
    const auto* source = static_cast<const BYTE*>(address);
    auto* destination = static_cast<BYTE*>(output);
    return read_resilient_impl(
        process,
        source,
        destination,
        size,
        0,
        spans,
        0);
}

template <typename T>
bool read_object(HANDLE process, const void* address, T& output) {
    return read(process, address, &output, sizeof(T)) == sizeof(T);
}

std::optional<std::string> read_ascii_string(
    HANDLE process,
    const void* address,
    size_t maximum_length = 1024) {
    std::string output;
    output.reserve((std::min)(maximum_length, size_t{128}));

    const auto* cursor = static_cast<const BYTE*>(address);
    std::array<char, 64> block{};
    while (output.size() < maximum_length) {
        const size_t wanted = (std::min)(block.size(), maximum_length - output.size());
        const SIZE_T count = read(process, cursor + output.size(), block.data(), wanted);
        if (count == 0) {
            return std::nullopt;
        }

        for (SIZE_T i = 0; i < count; ++i) {
            if (block[i] == '\0') {
                return output;
            }
            const unsigned char character = static_cast<unsigned char>(block[i]);
            if (character < 0x20 || character > 0x7E) {
                return std::nullopt;
            }
            output.push_back(block[i]);
        }

        if (count < wanted) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

}  // namespace memory

template <typename T>
T align_up(T value, T alignment) {
    if (alignment == 0) {
        return value;
    }
    return static_cast<T>((value + alignment - 1) / alignment * alignment);
}

class PeImage {
public:
    explicit PeImage(std::vector<BYTE>& bytes) : bytes_(bytes) {}

    IMAGE_DOS_HEADER* dos() {
        if (bytes_.size() < sizeof(IMAGE_DOS_HEADER)) {
            return nullptr;
        }
        return reinterpret_cast<IMAGE_DOS_HEADER*>(bytes_.data());
    }

    IMAGE_NT_HEADERS64* nt() {
        IMAGE_DOS_HEADER* header = dos();
        if (header == nullptr || header->e_lfanew < 0) {
            return nullptr;
        }

        const size_t offset = static_cast<size_t>(header->e_lfanew);
        if (offset > bytes_.size() ||
            bytes_.size() - offset < sizeof(IMAGE_NT_HEADERS64)) {
            return nullptr;
        }
        return reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes_.data() + offset);
    }

    IMAGE_SECTION_HEADER* sections() {
        IMAGE_NT_HEADERS64* header = nt();
        if (header == nullptr) {
            return nullptr;
        }
        return IMAGE_FIRST_SECTION(header);
    }

    bool validate(std::string& error) {
        IMAGE_DOS_HEADER* dos_header = dos();
        if (dos_header == nullptr || dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
            error = "invalid DOS header";
            return false;
        }

        IMAGE_NT_HEADERS64* nt_header = nt();
        if (nt_header == nullptr || nt_header->Signature != IMAGE_NT_SIGNATURE) {
            error = "invalid NT header";
            return false;
        }
        if (nt_header->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt_header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            error = "only x64 PE32+ images are supported";
            return false;
        }
        if (nt_header->FileHeader.NumberOfSections == 0 ||
            nt_header->FileHeader.NumberOfSections > 96) {
            error = "invalid section count";
            return false;
        }
        if (nt_header->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
            error = "truncated optional header";
            return false;
        }

        const auto* section_table = reinterpret_cast<const BYTE*>(IMAGE_FIRST_SECTION(nt_header));
        const size_t section_offset = static_cast<size_t>(section_table - bytes_.data());
        const size_t section_bytes =
            static_cast<size_t>(nt_header->FileHeader.NumberOfSections) *
            sizeof(IMAGE_SECTION_HEADER);
        if (section_offset > bytes_.size() || bytes_.size() - section_offset < section_bytes) {
            error = "truncated section table";
            return false;
        }

        if (nt_header->OptionalHeader.FileAlignment == 0 ||
            nt_header->OptionalHeader.SectionAlignment == 0) {
            error = "invalid PE alignment";
            return false;
        }
        return true;
    }

    std::optional<DWORD> offset_to_rva(DWORD offset) {
        IMAGE_NT_HEADERS64* header = nt();
        IMAGE_SECTION_HEADER* section = sections();
        if (header == nullptr || section == nullptr) {
            return std::nullopt;
        }

        for (WORD index = 0; index < header->FileHeader.NumberOfSections; ++index) {
            const uint64_t start = section[index].PointerToRawData;
            const uint64_t end = start + section[index].SizeOfRawData;
            if (offset >= start && offset < end) {
                return section[index].VirtualAddress +
                    (offset - section[index].PointerToRawData);
            }
        }
        return std::nullopt;
    }

    std::optional<DWORD> rva_to_offset(DWORD rva, size_t required = 1) {
        IMAGE_NT_HEADERS64* header = nt();
        IMAGE_SECTION_HEADER* section = sections();
        if (header == nullptr || section == nullptr) {
            return std::nullopt;
        }

        if (rva < header->OptionalHeader.SizeOfHeaders) {
            if (static_cast<uint64_t>(rva) + required <= bytes_.size()) {
                return rva;
            }
            return std::nullopt;
        }

        for (WORD index = 0; index < header->FileHeader.NumberOfSections; ++index) {
            const uint64_t span = (std::max)(
                static_cast<uint64_t>(section[index].Misc.VirtualSize),
                static_cast<uint64_t>(section[index].SizeOfRawData));
            const uint64_t start = section[index].VirtualAddress;
            const uint64_t end = start + span;
            if (rva < start || static_cast<uint64_t>(rva) + required > end) {
                continue;
            }

            const uint64_t raw = section[index].PointerToRawData +
                (static_cast<uint64_t>(rva) - start);
            if (raw + required <= bytes_.size() &&
                raw + required <= static_cast<uint64_t>(section[index].PointerToRawData) +
                    section[index].SizeOfRawData) {
                return static_cast<DWORD>(raw);
            }
        }
        return std::nullopt;
    }

    std::vector<IMAGE_SECTION_HEADER> copy_sections() {
        std::vector<IMAGE_SECTION_HEADER> output;
        IMAGE_NT_HEADERS64* header = nt();
        IMAGE_SECTION_HEADER* section = sections();
        if (header == nullptr || section == nullptr) {
            return output;
        }
        output.assign(section, section + header->FileHeader.NumberOfSections);
        return output;
    }

    static std::string section_name(const IMAGE_SECTION_HEADER& section) {
        char name[9]{};
        std::memcpy(name, section.Name, 8);
        return name;
    }

private:
    std::vector<BYTE>& bytes_;
};

class RvaRanges {
public:
    void add(uint32_t begin, size_t size) {
        if (size == 0) {
            return;
        }
        const uint64_t end = static_cast<uint64_t>(begin) + size;
        if (end > (std::numeric_limits<uint32_t>::max)()) {
            return;
        }
        ranges_.emplace_back(begin, static_cast<uint32_t>(end));
        normalized_ = false;
    }

    bool contains(uint32_t begin, size_t size) {
        normalize();
        const uint64_t end64 = static_cast<uint64_t>(begin) + size;
        if (end64 > (std::numeric_limits<uint32_t>::max)()) {
            return false;
        }
        const uint32_t end = static_cast<uint32_t>(end64);

        const auto iterator = std::upper_bound(
            ranges_.begin(),
            ranges_.end(),
            begin,
            [](uint32_t value, const auto& range) { return value < range.first; });
        if (iterator == ranges_.begin()) {
            return false;
        }
        const auto& range = *std::prev(iterator);
        return begin >= range.first && end <= range.second;
    }

private:
    void normalize() {
        if (normalized_) {
            return;
        }
        std::sort(ranges_.begin(), ranges_.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& range : ranges_) {
            if (merged.empty() || range.first > merged.back().second) {
                merged.push_back(range);
            } else {
                merged.back().second = (std::max)(merged.back().second, range.second);
            }
        }
        ranges_.swap(merged);
        normalized_ = true;
    }

    std::vector<std::pair<uint32_t, uint32_t>> ranges_;
    bool normalized_ = true;
};

struct MemoryIssue {
    std::string section;
    uint32_t rva = 0;
    uint64_t address = 0;
    size_t requested = 0;
    size_t copied = 0;
    DWORD state = 0;
    DWORD protect = 0;
    DWORD type = 0;
};

struct CopyStats {
    size_t requested = 0;
    size_t copied = 0;
    size_t partial_pages = 0;
    size_t failed_pages = 0;
};

CopyStats copy_section_from_process(
    HANDLE process_handle,
    PVOID module_base,
    DWORD module_size,
    const IMAGE_SECTION_HEADER& section,
    double limit,
    std::vector<BYTE>& image,
    RvaRanges& copied_ranges,
    std::vector<MemoryIssue>& issues) {
    CopyStats stats;

    const uint64_t raw_start = section.PointerToRawData;
    if (raw_start == 0 || raw_start >= image.size() ||
        section.VirtualAddress >= module_size) {
        return stats;
    }

    const uint64_t raw_available = image.size() - raw_start;
    const uint64_t file_backed = (std::min)(
        static_cast<uint64_t>(section.SizeOfRawData), raw_available);
    const uint64_t virtual_size = section.Misc.VirtualSize == 0
        ? file_backed
        : static_cast<uint64_t>(section.Misc.VirtualSize);
    const uint64_t remote_available = module_size - section.VirtualAddress;
    const size_t copyable = static_cast<size_t>((std::min)(
        file_backed,
        (std::min)(virtual_size, remote_available)));
    if (copyable == 0) {
        return stats;
    }

    const size_t all_pages = (copyable + kPageSize - 1) / kPageSize;
    const size_t selected_pages = (std::min)(
        all_pages,
        static_cast<size_t>(std::ceil(static_cast<double>(all_pages) * limit)));
    const size_t selected_bytes = (std::min)(copyable, selected_pages * kPageSize);
    stats.requested = selected_bytes;

    const std::string name = PeImage::section_name(section);
    for (size_t offset = 0, page = 0; offset < selected_bytes;
         offset += kPageSize, ++page) {
        const SIZE_T chunk = (std::min)(kPageSize, selected_bytes - offset);
        auto* remote = static_cast<BYTE*>(module_base) + section.VirtualAddress + offset;
        auto* local = image.data() + section.PointerToRawData + offset;

        SIZE_T count = 0;
        std::vector<memory::ReadSpan> spans;
        const auto information = memory::query(process_handle, remote);
        if (information && memory::is_readable(*information)) {
            count = memory::read_resilient(
                process_handle,
                remote,
                local,
                chunk,
                spans);
        }

        for (const memory::ReadSpan& span : spans) {
            if (span.size == 0 ||
                span.offset > chunk ||
                span.size > chunk - span.offset) {
                continue;
            }
            copied_ranges.add(
                section.VirtualAddress +
                    static_cast<uint32_t>(offset + span.offset),
                span.size);
        }

        stats.copied += count;
        if (count == 0) {
            ++stats.failed_pages;
        } else if (count < chunk) {
            ++stats.partial_pages;
        }

        if (count < chunk) {
            MemoryIssue issue;
            issue.section = name;
            issue.rva = section.VirtualAddress + static_cast<uint32_t>(offset);
            issue.address = reinterpret_cast<uint64_t>(remote);
            issue.requested = chunk;
            issue.copied = count;
            if (information) {
                issue.state = information->State;
                issue.protect = information->Protect;
                issue.type = information->Type;
            }
            issues.push_back(std::move(issue));
        }

        console::progress(
            name,
            page + 1,
            selected_pages,
            stats.copied,
            stats.partial_pages,
            stats.failed_pages);
    }
    std::printf("\n");
    return stats;
}

size_t restore_relocations(
    std::vector<BYTE>& image,
    uintptr_t loaded_base,
    RvaRanges& copied_ranges) {
    PeImage pe(image);
    IMAGE_NT_HEADERS64* nt = pe.nt();
    if (nt == nullptr ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        return 0;
    }

    const intptr_t delta = static_cast<intptr_t>(loaded_base) -
        static_cast<intptr_t>(nt->OptionalHeader.ImageBase);
    if (delta == 0) {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_BASE_RELOCATION)) {
        return 0;
    }

    size_t restored = 0;
    uint32_t consumed = 0;
    while (consumed + sizeof(IMAGE_BASE_RELOCATION) <= directory.Size) {
        const uint32_t block_rva = directory.VirtualAddress + consumed;
        const auto block_offset = pe.rva_to_offset(block_rva, sizeof(IMAGE_BASE_RELOCATION));
        if (!block_offset) {
            break;
        }

        IMAGE_BASE_RELOCATION block{};
        std::memcpy(&block, image.data() + *block_offset, sizeof(block));
        if (block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block.SizeOfBlock > directory.Size - consumed) {
            break;
        }

        const uint32_t entry_bytes =
            block.SizeOfBlock -
            static_cast<uint32_t>(sizeof(IMAGE_BASE_RELOCATION));
        const uint32_t entry_count = entry_bytes / sizeof(WORD);
        const auto entries_offset = pe.rva_to_offset(
            block_rva + sizeof(IMAGE_BASE_RELOCATION), entry_bytes);
        if (!entries_offset) {
            break;
        }

        for (uint32_t index = 0; index < entry_count; ++index) {
            WORD entry = 0;
            std::memcpy(
                &entry,
                image.data() + *entries_offset + index * sizeof(WORD),
                sizeof(entry));
            const WORD type = entry >> 12;
            const WORD offset = entry & 0x0FFF;
            if (type != IMAGE_REL_BASED_DIR64) {
                continue;
            }

            const uint32_t target_rva = block.VirtualAddress + offset;
            if (!copied_ranges.contains(target_rva, sizeof(uint64_t))) {
                continue;
            }

            const auto target_offset = pe.rva_to_offset(target_rva, sizeof(uint64_t));
            if (!target_offset) {
                continue;
            }

            uint64_t value = 0;
            std::memcpy(&value, image.data() + *target_offset, sizeof(value));
            value -= static_cast<uint64_t>(delta);
            std::memcpy(image.data() + *target_offset, &value, sizeof(value));
            ++restored;
        }

        consumed += block.SizeOfBlock;
    }
    return restored;
}

namespace imports {

struct ExportSymbol {
    std::string module;
    std::string name;
};

struct FoundImport {
    uint64_t resolved_address = 0;
    uint32_t slot_rva = 0;
    std::string module;
    std::string name;
};

bool remote_rva_range(const MODULEENTRY32W& module, DWORD rva, size_t size) {
    return rva < module.modBaseSize &&
        static_cast<uint64_t>(rva) + size <= module.modBaseSize;
}

template <typename T>
bool read_remote_array(
    HANDLE process_handle,
    const MODULEENTRY32W& module,
    DWORD rva,
    size_t count,
    std::vector<T>& output) {
    if (count == 0 || count > 1'000'000 ||
        count > (std::numeric_limits<size_t>::max)() / sizeof(T) ||
        !remote_rva_range(module, rva, count * sizeof(T))) {
        return false;
    }

    output.resize(count);
    return memory::read(
               process_handle,
               module.modBaseAddr + rva,
               output.data(),
               output.size() * sizeof(T)) == output.size() * sizeof(T);
}

std::unordered_map<uint64_t, ExportSymbol> build_export_map(
    HANDLE process_handle,
    DWORD pid) {
    std::unordered_map<uint64_t, ExportSymbol> output;

    for (const MODULEENTRY32W& module : process::modules(pid)) {
        IMAGE_DOS_HEADER dos{};
        if (!memory::read_object(process_handle, module.modBaseAddr, dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
            static_cast<uint64_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
                module.modBaseSize) {
            continue;
        }

        IMAGE_NT_HEADERS64 nt{};
        if (!memory::read_object(
                process_handle,
                module.modBaseAddr + dos.e_lfanew,
                nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
            continue;
        }

        const IMAGE_DATA_DIRECTORY directory =
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 ||
            !remote_rva_range(module, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) {
            continue;
        }

        IMAGE_EXPORT_DIRECTORY exports{};
        if (!memory::read_object(
                process_handle,
                module.modBaseAddr + directory.VirtualAddress,
                exports) ||
            exports.NumberOfFunctions == 0 || exports.NumberOfNames == 0) {
            continue;
        }

        std::vector<DWORD> functions;
        std::vector<DWORD> names;
        std::vector<WORD> ordinals;
        if (!read_remote_array(
                process_handle,
                module,
                exports.AddressOfFunctions,
                exports.NumberOfFunctions,
                functions) ||
            !read_remote_array(
                process_handle,
                module,
                exports.AddressOfNames,
                exports.NumberOfNames,
                names) ||
            !read_remote_array(
                process_handle,
                module,
                exports.AddressOfNameOrdinals,
                exports.NumberOfNames,
                ordinals)) {
            continue;
        }

        const std::string module_name = text::to_utf8(module.szModule);
        for (DWORD index = 0; index < exports.NumberOfNames; ++index) {
            if (ordinals[index] >= functions.size() ||
                !remote_rva_range(module, names[index], 1)) {
                continue;
            }

            const DWORD function_rva = functions[ordinals[index]];
            if (function_rva == 0 || !remote_rva_range(module, function_rva, 1)) {
                continue;
            }

            const uint64_t forwarder_begin = directory.VirtualAddress;
            const uint64_t forwarder_end = forwarder_begin + directory.Size;
            if (function_rva >= forwarder_begin && function_rva < forwarder_end) {
                continue;
            }

            const auto function_name = memory::read_ascii_string(
                process_handle,
                module.modBaseAddr + names[index]);
            if (!function_name || function_name->empty()) {
                continue;
            }

            const uint64_t address =
                reinterpret_cast<uint64_t>(module.modBaseAddr) + function_rva;
            output.try_emplace(address, ExportSymbol{module_name, *function_name});
        }
    }
    return output;
}

std::vector<FoundImport> find_imports(
    std::vector<BYTE>& image,
    const std::unordered_map<uint64_t, ExportSymbol>& exports,
    RvaRanges& copied_ranges,
    bool aggressive) {
    std::vector<FoundImport> output;
    std::unordered_set<uint32_t> seen_rvas;
    PeImage pe(image);
    IMAGE_NT_HEADERS64* nt = pe.nt();
    if (nt == nullptr || exports.empty()) {
        return output;
    }

    auto add_candidate = [&](DWORD raw_offset) -> bool {
        if (static_cast<uint64_t>(raw_offset) + sizeof(uint64_t) > image.size()) {
            return false;
        }

        uint64_t address = 0;
        std::memcpy(&address, image.data() + raw_offset, sizeof(address));
        const auto symbol = exports.find(address);
        if (symbol == exports.end()) {
            return false;
        }

        const auto rva = pe.offset_to_rva(raw_offset);
        if (!rva || !copied_ranges.contains(*rva, sizeof(uint64_t))) {
            return false;
        }
        if (!seen_rvas.insert(*rva).second) {
            return true;
        }

        output.push_back({address, *rva, symbol->second.module, symbol->second.name});
        return true;
    };

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IAT) {
        const IMAGE_DATA_DIRECTORY iat =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
        if (iat.VirtualAddress != 0 && iat.Size >= sizeof(uint64_t)) {
            const auto raw = pe.rva_to_offset(iat.VirtualAddress, iat.Size);
            if (raw) {
                for (DWORD offset = 0; offset + sizeof(uint64_t) <= iat.Size;
                     offset += sizeof(uint64_t)) {
                    add_candidate(*raw + offset);
                }
            }
        }
    }

    const auto sections = pe.copy_sections();
    for (const IMAGE_SECTION_HEADER& section : sections) {
        if (section.PointerToRawData == 0 || section.SizeOfRawData < sizeof(uint64_t) ||
            section.PointerToRawData >= image.size()) {
            continue;
        }
        if (!aggressive && (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }

        const DWORD start = section.PointerToRawData;
        const DWORD end = static_cast<DWORD>((std::min)(
            static_cast<uint64_t>(start) + section.SizeOfRawData,
            static_cast<uint64_t>(image.size())));
        const DWORD aligned_start = align_up(start, static_cast<DWORD>(sizeof(uint64_t)));
        const std::string section_name = PeImage::section_name(section);
        const bool allow_single = aggressive;
        const bool conventional_import_section =
            section_name == ".idata" || section_name == ".rdata" ||
            section_name == ".data";

        std::vector<DWORD> run;
        auto commit_run = [&]() {
            if (allow_single || run.size() >= (conventional_import_section ? 2u : 3u)) {
                for (DWORD raw_offset : run) {
                    add_candidate(raw_offset);
                }
            }
            run.clear();
        };

        for (DWORD raw_offset = aligned_start;
             static_cast<uint64_t>(raw_offset) + sizeof(uint64_t) <= end;
             raw_offset += sizeof(uint64_t)) {
            uint64_t address = 0;
            std::memcpy(&address, image.data() + raw_offset, sizeof(address));
            if (exports.contains(address)) {
                run.push_back(raw_offset);
            } else {
                commit_run();
            }
        }
        commit_run();
    }

    std::sort(output.begin(), output.end(), [](const FoundImport& lhs, const FoundImport& rhs) {
        return lhs.slot_rva < rhs.slot_rva;
    });
    return output;
}

size_t patch_rip_relative_references(
    std::vector<BYTE>& image,
    const std::unordered_map<uint32_t, uint32_t>& old_iat_to_new_iat,
    std::unordered_set<uint32_t>& patched_slots) {
    if (old_iat_to_new_iat.empty()) {
        return 0;
    }

    PeImage pe(image);
    const auto sections = pe.copy_sections();
    size_t patched = 0;

    for (const IMAGE_SECTION_HEADER& section : sections) {
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.PointerToRawData == 0 || section.SizeOfRawData < 6 ||
            section.PointerToRawData >= image.size()) {
            continue;
        }

        const DWORD start = section.PointerToRawData;
        const DWORD end = static_cast<DWORD>((std::min)(
            static_cast<uint64_t>(start) + section.SizeOfRawData,
            static_cast<uint64_t>(image.size())));

        for (DWORD position = start; position + 6 <= end; ++position) {
            DWORD opcode = position;
            if ((image[opcode] & 0xF0) == 0x40) {
                ++opcode;
            }
            if (opcode + 6 > end) {
                continue;
            }

            size_t instruction_size = 0;
            size_t displacement_offset = 0;
            const BYTE operation = image[opcode];
            const BYTE modrm = image[opcode + 1];

            if (operation == 0xFF && (modrm == 0x15 || modrm == 0x25)) {
                displacement_offset = opcode + 2;
                instruction_size = (opcode - position) + 6;
            } else if ((operation == 0x8B || operation == 0x8D) &&
                       (modrm & 0xC7) == 0x05) {
                displacement_offset = opcode + 2;
                instruction_size = (opcode - position) + 6;
            } else {
                continue;
            }

            int32_t displacement = 0;
            std::memcpy(
                &displacement,
                image.data() + displacement_offset,
                sizeof(displacement));

            const auto instruction_rva = pe.offset_to_rva(position);
            if (!instruction_rva) {
                continue;
            }

            const int64_t next_rva =
                static_cast<int64_t>(*instruction_rva) +
                static_cast<int64_t>(instruction_size);
            const int64_t target = next_rva + displacement;
            if (target < 0 || target > (std::numeric_limits<uint32_t>::max)()) {
                continue;
            }

            const auto replacement = old_iat_to_new_iat.find(
                static_cast<uint32_t>(target));
            if (replacement == old_iat_to_new_iat.end()) {
                continue;
            }

            const int64_t new_displacement =
                static_cast<int64_t>(replacement->second) - next_rva;
            if (new_displacement < (std::numeric_limits<int32_t>::min)() ||
                new_displacement > (std::numeric_limits<int32_t>::max)()) {
                continue;
            }

            const int32_t encoded = static_cast<int32_t>(new_displacement);
            std::memcpy(
                image.data() + displacement_offset,
                &encoded,
                sizeof(encoded));
            patched_slots.insert(static_cast<uint32_t>(target));
            ++patched;
        }
    }
    return patched;
}

struct GroupLayout {
    std::string module;
    std::vector<const FoundImport*> entries;
    DWORD ilt_offset = 0;
    DWORD iat_offset = 0;
    DWORD module_name_offset = 0;
    std::vector<DWORD> import_name_offsets;
};

bool rebuild(
    std::vector<BYTE>& image,
    const std::vector<FoundImport>& found,
    size_t& patched_references,
    size_t& patched_slots,
    std::string& error) {
    patched_references = 0;
    patched_slots = 0;
    if (found.empty()) {
        error = "no resolved imports were found";
        return false;
    }

    std::map<std::string, std::vector<const FoundImport*>, std::less<>> grouped;
    for (const FoundImport& import : found) {
        grouped[import.module].push_back(&import);
    }

    std::vector<GroupLayout> groups;
    groups.reserve(grouped.size());
    for (auto& [module, entries] : grouped) {
        std::sort(entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->slot_rva < rhs->slot_rva;
        });
        GroupLayout layout;
        layout.module = module;
        layout.entries = std::move(entries);
        groups.push_back(std::move(layout));
    }

    PeImage pe(image);
    IMAGE_NT_HEADERS64* nt = pe.nt();
    IMAGE_SECTION_HEADER* sections = pe.sections();
    if (nt == nullptr || sections == nullptr) {
        error = "invalid PE headers";
        return false;
    }

    const DWORD file_alignment = nt->OptionalHeader.FileAlignment;
    const DWORD section_alignment = nt->OptionalHeader.SectionAlignment;
    if (file_alignment == 0 || section_alignment == 0) {
        error = "invalid PE alignment";
        return false;
    }

    const size_t section_table_offset = static_cast<size_t>(
        reinterpret_cast<BYTE*>(sections) - image.data());
    const size_t next_header_end = section_table_offset +
        static_cast<size_t>(nt->FileHeader.NumberOfSections + 1) *
            sizeof(IMAGE_SECTION_HEADER);

    DWORD first_raw = (std::numeric_limits<DWORD>::max)();
    uint64_t highest_virtual_end = 0;
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (sections[index].PointerToRawData != 0) {
            first_raw = (std::min)(first_raw, sections[index].PointerToRawData);
        }
        highest_virtual_end = (std::max)(
            highest_virtual_end,
            static_cast<uint64_t>(sections[index].VirtualAddress) +
                (std::max)(sections[index].Misc.VirtualSize, sections[index].SizeOfRawData));
    }

    if (first_raw == (std::numeric_limits<DWORD>::max)() || next_header_end > first_raw) {
        error = "no room for another section header";
        return false;
    }

    const uint64_t expanded_headers = align_up<uint64_t>(next_header_end, file_alignment);
    if (expanded_headers > first_raw || expanded_headers > (std::numeric_limits<DWORD>::max)()) {
        error = "section header would overlap the first section";
        return false;
    }

    uint64_t cursor =
        static_cast<uint64_t>(groups.size() + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    cursor = align_up<uint64_t>(cursor, alignof(IMAGE_THUNK_DATA64));

    for (GroupLayout& group : groups) {
        group.ilt_offset = static_cast<DWORD>(cursor);
        cursor += static_cast<uint64_t>(group.entries.size() + 1) *
            sizeof(IMAGE_THUNK_DATA64);
    }

    const uint64_t iat_begin = cursor;
    for (GroupLayout& group : groups) {
        group.iat_offset = static_cast<DWORD>(cursor);
        cursor += static_cast<uint64_t>(group.entries.size() + 1) *
            sizeof(IMAGE_THUNK_DATA64);
    }
    const uint64_t iat_end = cursor;

    for (GroupLayout& group : groups) {
        group.module_name_offset = static_cast<DWORD>(cursor);
        cursor += group.module.size() + 1;
        group.import_name_offsets.reserve(group.entries.size());
        for (const FoundImport* import : group.entries) {
            cursor = align_up<uint64_t>(cursor, alignof(WORD));
            group.import_name_offsets.push_back(static_cast<DWORD>(cursor));
            cursor += sizeof(WORD) + import->name.size() + 1;
        }
    }

    if (cursor > (std::numeric_limits<DWORD>::max)()) {
        error = "rebuilt import section is too large";
        return false;
    }

    const DWORD virtual_size = static_cast<DWORD>(cursor);
    const uint64_t raw_size64 = align_up<uint64_t>(virtual_size, file_alignment);
    const uint64_t new_raw64 = align_up<uint64_t>(image.size(), file_alignment);
    const uint64_t new_rva64 = align_up<uint64_t>(highest_virtual_end, section_alignment);
    if (raw_size64 > (std::numeric_limits<DWORD>::max)() ||
        new_raw64 > (std::numeric_limits<DWORD>::max)() ||
        new_rva64 > (std::numeric_limits<DWORD>::max)() ||
        new_raw64 + raw_size64 > (std::numeric_limits<DWORD>::max)() ||
        new_rva64 + virtual_size > (std::numeric_limits<DWORD>::max)()) {
        error = "rebuilt section exceeds PE32+ limits";
        return false;
    }

    const DWORD raw_size = static_cast<DWORD>(raw_size64);
    const DWORD new_raw = static_cast<DWORD>(new_raw64);
    const DWORD new_rva = static_cast<DWORD>(new_rva64);
    image.resize(static_cast<size_t>(new_raw64 + raw_size64), 0);

    PeImage resized_pe(image);
    nt = resized_pe.nt();
    sections = resized_pe.sections();
    if (nt == nullptr || sections == nullptr) {
        error = "PE headers became invalid after resizing";
        return false;
    }

    IMAGE_SECTION_HEADER* new_section =
        &sections[nt->FileHeader.NumberOfSections];
    *new_section = {};
    constexpr char section_name[] = ".dcrypt";
    std::memcpy(new_section->Name, section_name, sizeof(section_name) - 1);
    new_section->Misc.VirtualSize = virtual_size;
    new_section->VirtualAddress = new_rva;
    new_section->SizeOfRawData = raw_size;
    new_section->PointerToRawData = new_raw;
    new_section->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    ++nt->FileHeader.NumberOfSections;
    nt->OptionalHeader.SizeOfHeaders = static_cast<DWORD>((std::max)(
        static_cast<uint64_t>(nt->OptionalHeader.SizeOfHeaders),
        expanded_headers));
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(align_up<uint64_t>(
        static_cast<uint64_t>(new_rva) + virtual_size,
        section_alignment));
    if (static_cast<uint64_t>(nt->OptionalHeader.SizeOfInitializedData) + raw_size <=
        (std::numeric_limits<DWORD>::max)()) {
        nt->OptionalHeader.SizeOfInitializedData += raw_size;
    }
    nt->OptionalHeader.CheckSum = 0;

    BYTE* data = image.data() + new_raw;
    auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(data);
    std::unordered_map<uint32_t, uint32_t> old_iat_to_new_iat;

    for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
        GroupLayout& group = groups[group_index];
        IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[group_index];
        descriptor.OriginalFirstThunk = new_rva + group.ilt_offset;
        descriptor.FirstThunk = new_rva + group.iat_offset;
        descriptor.Name = new_rva + group.module_name_offset;

        std::memcpy(
            data + group.module_name_offset,
            group.module.c_str(),
            group.module.size() + 1);

        auto* ilt = reinterpret_cast<IMAGE_THUNK_DATA64*>(data + group.ilt_offset);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(data + group.iat_offset);
        for (size_t index = 0; index < group.entries.size(); ++index) {
            const FoundImport& import = *group.entries[index];
            const DWORD import_name_offset = group.import_name_offsets[index];
            const DWORD import_name_rva = new_rva + import_name_offset;
            ilt[index].u1.AddressOfData = import_name_rva;
            iat[index].u1.AddressOfData = import_name_rva;

            const WORD hint = 0;
            std::memcpy(data + import_name_offset, &hint, sizeof(hint));
            std::memcpy(
                data + import_name_offset + sizeof(hint),
                import.name.c_str(),
                import.name.size() + 1);

            old_iat_to_new_iat[import.slot_rva] =
                new_rva + group.iat_offset +
                static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64));
        }
    }

    const DWORD descriptor_size = static_cast<DWORD>(
        (groups.size() + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    nt->OptionalHeader.NumberOfRvaAndSizes = (std::max)(
        nt->OptionalHeader.NumberOfRvaAndSizes,
        static_cast<DWORD>(IMAGE_DIRECTORY_ENTRY_IAT + 1));
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
        new_rva,
        descriptor_size,
    };
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {
        new_rva + static_cast<DWORD>(iat_begin),
        static_cast<DWORD>(iat_end - iat_begin),
    };
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {};

    std::unordered_set<uint32_t> redirected_slots;
    patched_references = patch_rip_relative_references(
        image,
        old_iat_to_new_iat,
        redirected_slots);
    patched_slots = redirected_slots.size();
    return true;
}

}  // namespace imports

bool copy_rva_range(
    std::vector<BYTE>& destination,
    std::vector<BYTE>& source,
    DWORD rva,
    size_t size) {
    if (size == 0) {
        return true;
    }

    PeImage destination_pe(destination);
    PeImage source_pe(source);
    const auto destination_offset = destination_pe.rva_to_offset(rva, size);
    const auto source_offset = source_pe.rva_to_offset(rva, size);
    if (!destination_offset || !source_offset) {
        return false;
    }

    std::memcpy(
        destination.data() + *destination_offset,
        source.data() + *source_offset,
        size);
    return true;
}

bool copy_rva_string(
    std::vector<BYTE>& destination,
    std::vector<BYTE>& source,
    DWORD rva,
    size_t maximum_length = 4096) {
    PeImage source_pe(source);
    const auto source_offset = source_pe.rva_to_offset(rva, 1);
    if (!source_offset) {
        return false;
    }

    size_t length = 0;
    while (length < maximum_length &&
           static_cast<uint64_t>(*source_offset) + length < source.size()) {
        if (source[*source_offset + length] == 0) {
            return copy_rva_range(destination, source, rva, length + 1);
        }
        ++length;
    }
    return false;
}

bool restore_original_imports(
    std::vector<BYTE>& image,
    std::vector<BYTE>& disk_image,
    std::string& error) {
    PeImage current(image);
    PeImage original(disk_image);
    IMAGE_NT_HEADERS64* current_nt = current.nt();
    IMAGE_NT_HEADERS64* original_nt = original.nt();
    if (current_nt == nullptr || original_nt == nullptr) {
        error = "invalid PE while restoring original imports";
        return false;
    }

    if (original_nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return true;
    }

    const IMAGE_DATA_DIRECTORY import_directory =
        original_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_DATA_DIRECTORY iat_directory =
        original_nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IAT
        ? original_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT]
        : IMAGE_DATA_DIRECTORY{};

    current_nt->OptionalHeader.NumberOfRvaAndSizes = (std::min)(
        static_cast<DWORD>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES),
        (std::max)(
            current_nt->OptionalHeader.NumberOfRvaAndSizes,
            original_nt->OptionalHeader.NumberOfRvaAndSizes));
    current_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] =
        import_directory;
    current_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] =
        iat_directory;
    current_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};

    if (import_directory.VirtualAddress == 0 ||
        import_directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return true;
    }

    constexpr size_t kMaximumDescriptors = 4096;
    constexpr size_t kMaximumThunksPerDescriptor = 1'000'000;

    for (size_t descriptor_index = 0;
         descriptor_index < kMaximumDescriptors;
         ++descriptor_index) {
        const uint64_t descriptor_rva64 =
            static_cast<uint64_t>(import_directory.VirtualAddress) +
            descriptor_index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (descriptor_rva64 > (std::numeric_limits<DWORD>::max)()) {
            error = "original import descriptor RVA overflow";
            return false;
        }

        const DWORD descriptor_rva = static_cast<DWORD>(descriptor_rva64);
        const auto descriptor_offset =
            original.rva_to_offset(descriptor_rva, sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!descriptor_offset) {
            error = "original import descriptor is outside the image";
            return false;
        }

        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(
            &descriptor,
            disk_image.data() + *descriptor_offset,
            sizeof(descriptor));

        if (!copy_rva_range(
                image,
                disk_image,
                descriptor_rva,
                sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
            error = "failed to restore an original import descriptor";
            return false;
        }

        if (descriptor.OriginalFirstThunk == 0 &&
            descriptor.FirstThunk == 0 &&
            descriptor.Name == 0) {
            return true;
        }

        if (descriptor.Name != 0 &&
            !copy_rva_string(image, disk_image, descriptor.Name)) {
            error = "failed to restore an imported module name";
            return false;
        }

        const DWORD lookup_rva = descriptor.OriginalFirstThunk != 0
            ? descriptor.OriginalFirstThunk
            : descriptor.FirstThunk;

        for (size_t thunk_index = 0;
             thunk_index < kMaximumThunksPerDescriptor;
             ++thunk_index) {
            const uint64_t lookup_entry_rva64 =
                static_cast<uint64_t>(lookup_rva) +
                thunk_index * sizeof(IMAGE_THUNK_DATA64);
            const uint64_t iat_entry_rva64 =
                static_cast<uint64_t>(descriptor.FirstThunk) +
                thunk_index * sizeof(IMAGE_THUNK_DATA64);
            if (lookup_entry_rva64 > (std::numeric_limits<DWORD>::max)() ||
                iat_entry_rva64 > (std::numeric_limits<DWORD>::max)()) {
                error = "original import thunk RVA overflow";
                return false;
            }

            const DWORD lookup_entry_rva =
                static_cast<DWORD>(lookup_entry_rva64);
            const DWORD iat_entry_rva = static_cast<DWORD>(iat_entry_rva64);
            const auto lookup_offset = original.rva_to_offset(
                lookup_entry_rva,
                sizeof(IMAGE_THUNK_DATA64));
            if (!lookup_offset) {
                error = "original import lookup table is outside the image";
                return false;
            }

            IMAGE_THUNK_DATA64 thunk{};
            std::memcpy(
                &thunk,
                disk_image.data() + *lookup_offset,
                sizeof(thunk));

            if (!copy_rva_range(
                    image,
                    disk_image,
                    lookup_entry_rva,
                    sizeof(IMAGE_THUNK_DATA64)) ||
                !copy_rva_range(
                    image,
                    disk_image,
                    iat_entry_rva,
                    sizeof(IMAGE_THUNK_DATA64))) {
                error = "failed to restore original import thunks";
                return false;
            }

            if (thunk.u1.AddressOfData == 0) {
                break;
            }

            if ((thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) == 0) {
                if (thunk.u1.AddressOfData >
                    (std::numeric_limits<DWORD>::max)()) {
                    error = "original import-by-name RVA overflow";
                    return false;
                }

                const DWORD name_rva =
                    static_cast<DWORD>(thunk.u1.AddressOfData);
                if (!copy_rva_range(
                        image,
                        disk_image,
                        name_rva,
                        sizeof(WORD)) ||
                    !copy_rva_string(
                        image,
                        disk_image,
                        name_rva + sizeof(WORD))) {
                    error = "failed to restore an original import name";
                    return false;
                }
            }
        }
    }

    error = "original import descriptor list is not terminated";
    return false;
}

bool write_minidump(
    HANDLE process_handle,
    DWORD pid,
    const std::filesystem::path& path,
    std::string& error) {
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path(),
            filesystem_error);
        if (filesystem_error) {
            error = "cannot create minidump directory: " +
                filesystem_error.message();
            return false;
        }
    }

    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file) {
        error = "cannot create minidump: " + text::win32_error();
        return false;
    }

    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithUnloadedModules |
        MiniDumpWithFullMemoryInfo |
        MiniDumpWithThreadInfo |
        MiniDumpWithCodeSegs);

    if (!MiniDumpWriteDump(
            process_handle,
            pid,
            file.get(),
            type,
            nullptr,
            nullptr,
            nullptr)) {
        error = "MiniDumpWriteDump failed: " + text::win32_error();
        return false;
    }
    return true;
}

std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 16);
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20) {
                char buffer[7]{};
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "\\u%04X",
                    static_cast<unsigned>(character));
                output += buffer;
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

bool write_json_report(
    const std::filesystem::path& path,
    DWORD pid,
    const process::ModuleInfo& module,
    const Options& options,
    const CopyStats& code,
    const CopyStats& data,
    const std::vector<MemoryIssue>& issues,
    size_t restored_relocations,
    size_t found_imports,
    bool imports_rebuilt,
    size_t patched_references,
    size_t patched_slots,
    std::string& error) {
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path(),
            filesystem_error);
        if (filesystem_error) {
            error = "cannot create report directory: " +
                filesystem_error.message();
            return false;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot create JSON report";
        return false;
    }

    const double code_coverage = code.requested == 0
        ? 1.0
        : static_cast<double>(code.copied) /
            static_cast<double>(code.requested);
    const double data_coverage = data.requested == 0
        ? 1.0
        : static_cast<double>(data.copied) /
            static_cast<double>(data.requested);

    stream << "{\n";
    stream << "  \"tool\": \"decrypton\",\n";
    stream << "  \"version\": \"" << kVersion << "\",\n";
    stream << "  \"pid\": " << pid << ",\n";
    stream << "  \"module\": {\n";
    stream << "    \"name\": \"" <<
        json_escape(text::to_utf8(module.name)) << "\",\n";
    stream << "    \"path\": \"" <<
        json_escape(text::to_utf8(module.path.wstring())) << "\",\n";
    stream << "    \"base\": \"0x" << std::hex <<
        reinterpret_cast<uint64_t>(module.base) << std::dec << "\",\n";
    stream << "    \"size\": " << module.size << "\n";
    stream << "  },\n";
    stream << "  \"options\": {\n";
    stream << "    \"code_limit\": " << options.code_limit << ",\n";
    stream << "    \"minimum_code_coverage\": " <<
        options.minimum_code_coverage << ",\n";
    stream << "    \"dump_data\": " <<
        (options.dump_data ? "true" : "false") << ",\n";
    stream << "    \"rebuild_imports\": " <<
        (options.rebuild_imports ? "true" : "false") << ",\n";
    stream << "    \"aggressive_import_scan\": " <<
        (options.aggressive_import_scan ? "true" : "false") << "\n";
    stream << "  },\n";
    stream << "  \"coverage\": {\n";
    stream << "    \"code_requested\": " << code.requested << ",\n";
    stream << "    \"code_copied\": " << code.copied << ",\n";
    stream << "    \"code_ratio\": " << code_coverage << ",\n";
    stream << "    \"code_partial_pages\": " << code.partial_pages << ",\n";
    stream << "    \"code_failed_pages\": " << code.failed_pages << ",\n";
    stream << "    \"data_requested\": " << data.requested << ",\n";
    stream << "    \"data_copied\": " << data.copied << ",\n";
    stream << "    \"data_ratio\": " << data_coverage << ",\n";
    stream << "    \"data_partial_pages\": " << data.partial_pages << ",\n";
    stream << "    \"data_failed_pages\": " << data.failed_pages << "\n";
    stream << "  },\n";
    stream << "  \"relocations_restored\": " <<
        restored_relocations << ",\n";
    stream << "  \"imports\": {\n";
    stream << "    \"found_slots\": " << found_imports << ",\n";
    stream << "    \"rebuilt\": " <<
        (imports_rebuilt ? "true" : "false") << ",\n";
    stream << "    \"patched_references\": " <<
        patched_references << ",\n";
    stream << "    \"redirected_slots\": " <<
        patched_slots << "\n";
    stream << "  },\n";
    stream << "  \"memory_issues\": [\n";

    for (size_t index = 0; index < issues.size(); ++index) {
        const MemoryIssue& issue = issues[index];
        stream << "    {\"section\":\"" <<
            json_escape(issue.section) <<
            "\",\"rva\":\"0x" << std::hex << issue.rva <<
            "\",\"address\":\"0x" << issue.address <<
            "\",\"requested\":" << std::dec << issue.requested <<
            ",\"copied\":" << issue.copied <<
            ",\"state\":\"0x" << std::hex << issue.state <<
            "\",\"protect\":\"0x" << issue.protect <<
            "\",\"type\":\"0x" << issue.type << std::dec << "}";
        if (index + 1 != issues.size()) {
            stream << ",";
        }
        stream << "\n";
    }

    stream << "  ]\n";
    stream << "}\n";
    stream.flush();
    if (!stream) {
        error = "failed while writing JSON report";
        return false;
    }
    return true;
}


bool read_file(
    const std::filesystem::path& path,
    std::vector<BYTE>& output,
    std::string& error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "cannot open input image";
        return false;
    }

    const std::streamoff length = stream.tellg();
    if (length <= 0 ||
        static_cast<uint64_t>(length) > (std::numeric_limits<size_t>::max)()) {
        error = "invalid input image size";
        return false;
    }

    output.resize(static_cast<size_t>(length));
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(output.data()),
        static_cast<std::streamsize>(length));
    if (!stream) {
        error = "failed to read the complete input image";
        return false;
    }
    return true;
}

bool write_file(
    const std::filesystem::path& path,
    const std::vector<BYTE>& image,
    std::string& error) {
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "cannot create output directory: " + filesystem_error.message();
            return false;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot create output file";
        return false;
    }

    stream.write(
        reinterpret_cast<const char*>(image.data()),
        static_cast<std::streamsize>(image.size()));
    stream.flush();
    if (!stream) {
        error = "failed while writing the output file";
        return false;
    }
    return true;
}

std::filesystem::path default_output_name(std::wstring_view module_name) {
    std::filesystem::path path{std::wstring(module_name)};
    std::wstring stem = path.stem().wstring();
    std::wstring extension = path.extension().wstring();
    if (stem.empty()) {
        stem = L"decrypton";
    }
    if (extension.empty()) {
        extension = L".bin";
    }
    return stem + L"-dumped" + extension;
}

std::filesystem::path default_report_name(
    const std::filesystem::path& output) {
    std::filesystem::path report = output;
    report += L".json";
    return report;
}

}  // namespace

int main(int argc, char** argv) {
    console::initialize();
    console::banner();

    std::string error;
    const auto parsed = parse_options(argc, argv, error);
    if (!parsed) {
        console::failure("%s", error.c_str());
        std::printf("\n");
        print_usage();
        return 2;
    }

    Options options = *parsed;
    if (options.show_help) {
        print_usage();
        return 0;
    }
    if (options.show_version) {
        std::printf("decrypton %.*s\n",
            static_cast<int>(kVersion.size()),
            kVersion.data());
        return 0;
    }

    const std::wstring process_name = text::to_wide(options.process_name);
    if (!options.process_name.empty() && process_name.empty()) {
        console::failure("process name is not valid UTF-8");
        return 2;
    }
    if (!options.pid && process_name.empty()) {
        console::failure("a process name or PID is required");
        return 2;
    }

    const DWORD pid = options.pid.value_or(process::find(process_name));
    if (pid == 0) {
        console::failure("target process is not running");
        return 1;
    }

    UniqueHandle process_handle(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        pid));
    if (!process_handle) {
        console::failure(
            "OpenProcess failed: %s",
            text::win32_error().c_str());
        return 1;
    }

    if (options.list_modules) {
        const auto loaded_modules = process::modules(pid);
        if (loaded_modules.empty()) {
            console::failure("no modules could be enumerated");
            return 1;
        }

        std::printf("%-18s %-12s %-28s %s\n", "Base", "Size", "Module", "Path");
        for (const MODULEENTRY32W& entry : loaded_modules) {
            std::printf(
                "0x%016llX %-12lu %-28s %s\n",
                static_cast<unsigned long long>(
                    reinterpret_cast<uintptr_t>(entry.modBaseAddr)),
                static_cast<unsigned long>(entry.modBaseSize),
                text::to_utf8(entry.szModule).c_str(),
                text::to_utf8(entry.szExePath).c_str());
        }
        return 0;
    }

    const std::wstring requested_module =
        text::to_wide(options.module_name);
    if (!options.module_name.empty() && requested_module.empty()) {
        console::failure("module name is not valid UTF-8");
        return 2;
    }

    std::wstring preferred_module;
    if (!requested_module.empty()) {
        preferred_module = requested_module;
    } else if (!options.pid) {
        preferred_module = process_name;
    }

    const auto module = process::main_module(pid, preferred_module);
    if (!module) {
        console::failure(
            preferred_module.empty()
                ? "main module lookup failed"
                : "requested module was not found");
        return 1;
    }

    if (options.output.empty()) {
        options.output = default_output_name(module->name);
    }
    if (options.write_report && options.report.empty()) {
        options.report = default_report_name(options.output);
    }

    console::info("PID: %lu\n", pid);
    console::info(
        "module: %s @ %p (%lu bytes)\n",
        text::to_utf8(module->name).c_str(),
        module->base,
        module->size);
    console::info(
        "disk image: %s\n",
        text::to_utf8(module->path.wstring()).c_str());
    console::info(
        "code limit: %.2f%%\n",
        options.code_limit * 100.0);
    console::info(
        "minimum import coverage: %.2f%%\n",
        options.minimum_code_coverage * 100.0);
    console::info(
        "output: %s\n",
        text::to_utf8(options.output.wstring()).c_str());
    if (options.write_report) {
        console::info(
            "report: %s\n",
            text::to_utf8(options.report.wstring()).c_str());
    }

    std::vector<BYTE> image;
    if (!read_file(module->path, image, error)) {
        console::failure("%s", error.c_str());
        return 1;
    }
    std::vector<BYTE> disk_image = image;

    PeImage pe(image);
    if (!pe.validate(error)) {
        console::failure("invalid input PE: %s", error.c_str());
        return 1;
    }
    console::success("loaded %zu bytes from disk", image.size());

    const auto sections = pe.copy_sections();
    RvaRanges copied_ranges;
    CopyStats code_total;
    CopyStats data_total;
    std::vector<MemoryIssue> memory_issues;

    console::info("copying executable sections\n");
    for (const IMAGE_SECTION_HEADER& section : sections) {
        const bool executable =
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
            (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0;
        if (!executable) {
            continue;
        }

        const CopyStats stats = copy_section_from_process(
            process_handle.get(),
            module->base,
            module->size,
            section,
            options.code_limit,
            image,
            copied_ranges,
            memory_issues);
        code_total.requested += stats.requested;
        code_total.copied += stats.copied;
        code_total.partial_pages += stats.partial_pages;
        code_total.failed_pages += stats.failed_pages;
    }

    if (options.dump_data) {
        console::info("copying non-executable sections\n");
        for (const IMAGE_SECTION_HEADER& section : sections) {
            const bool executable =
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
                (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0;
            if (executable) {
                continue;
            }

            const CopyStats stats = copy_section_from_process(
                process_handle.get(),
                module->base,
                module->size,
                section,
                1.0,
                image,
                copied_ranges,
                memory_issues);
            data_total.requested += stats.requested;
            data_total.copied += stats.copied;
            data_total.partial_pages += stats.partial_pages;
            data_total.failed_pages += stats.failed_pages;
        }
    }

    const double code_coverage = code_total.requested == 0
        ? 1.0
        : static_cast<double>(code_total.copied) /
            static_cast<double>(code_total.requested);

    console::success(
        "code copied: %zu / %zu bytes (%.2f%%, %zu partial, %zu failed pages)",
        code_total.copied,
        code_total.requested,
        code_coverage * 100.0,
        code_total.partial_pages,
        code_total.failed_pages);
    if (options.dump_data) {
        const double data_coverage = data_total.requested == 0
            ? 1.0
            : static_cast<double>(data_total.copied) /
                static_cast<double>(data_total.requested);
        console::success(
            "data copied: %zu / %zu bytes (%.2f%%, %zu partial, %zu failed pages)",
            data_total.copied,
            data_total.requested,
            data_coverage * 100.0,
            data_total.partial_pages,
            data_total.failed_pages);
    }

    if (code_coverage < 1.0) {
        console::warning(
            "output is a hybrid image; unreadable fragments were preserved from disk");
    }

    const size_t restored_relocations = restore_relocations(
        image,
        reinterpret_cast<uintptr_t>(module->base),
        copied_ranges);
    if (restored_relocations > 0) {
        console::success(
            "restored %zu relocated addresses",
            restored_relocations);
    } else {
        console::info(
            "no copied relocation entries required normalization\n");
    }

    size_t found_imports = 0;
    size_t patched_references = 0;
    size_t patched_slots = 0;
    bool imports_rebuilt = false;

    auto restore_imports = [&]() {
        std::string restore_error;
        if (restore_original_imports(
                image,
                disk_image,
                restore_error)) {
            console::success("preserved original import structures");
            return true;
        }

        console::warning(
            "could not fully restore original imports: %s",
            restore_error.c_str());
        return false;
    };

    if (!options.rebuild_imports) {
        restore_imports();
    } else if (code_coverage < options.minimum_code_coverage &&
               !options.force_import_rebuild) {
        console::warning(
            "import rebuild skipped: code coverage %.2f%% is below %.2f%%",
            code_coverage * 100.0,
            options.minimum_code_coverage * 100.0);
        restore_imports();
    } else {
        console::info("building loaded export map\n");
        const auto exports =
            imports::build_export_map(process_handle.get(), pid);
        console::success(
            "indexed %zu named exports",
            exports.size());

        console::info(
            "scanning resolved imports (%s mode)\n",
            options.aggressive_import_scan ? "aggressive" : "safe");
        const auto found = imports::find_imports(
            image,
            exports,
            copied_ranges,
            options.aggressive_import_scan);
        found_imports = found.size();
        console::success(
            "found %zu copied import slots",
            found_imports);

        if (!found.empty()) {
            std::map<std::string, size_t, std::less<>> module_counts;
            for (const auto& import : found) {
                ++module_counts[import.module];
            }
            for (const auto& [name, count] : module_counts) {
                std::printf("  %-28s %zu\n", name.c_str(), count);
            }

            const std::vector<BYTE> before_rebuild = image;
            error.clear();
            if (imports::rebuild(
                    image,
                    found,
                    patched_references,
                    patched_slots,
                    error)) {
                if (patched_slots != found_imports &&
                    !options.force_import_rebuild) {
                    image = before_rebuild;
                    console::warning(
                        "rebuilt table rejected: redirected %zu / %zu import slots",
                        patched_slots,
                        found_imports);
                    restore_imports();
                } else {
                    imports_rebuilt = true;
                    console::success(
                        "rebuilt imports: %zu redirected slots, %zu RIP-relative references",
                        patched_slots,
                        patched_references);
                }
            } else {
                image = before_rebuild;
                console::warning(
                    "import rebuild skipped: %s",
                    error.c_str());
                restore_imports();
            }
        } else {
            console::warning(
                "no reliable imports found; preserving the original directory");
            restore_imports();
        }
    }

    PeImage final_pe(image);
    if (!final_pe.validate(error)) {
        console::failure(
            "output validation failed: %s",
            error.c_str());
        return 1;
    }

    IMAGE_NT_HEADERS64* final_nt = final_pe.nt();
    if (final_nt != nullptr) {
        final_nt->OptionalHeader.CheckSum = 0;
        if (final_nt->OptionalHeader.NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_SECURITY) {
            final_nt->OptionalHeader
                .DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {};
        }
        if (final_nt->OptionalHeader.NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT) {
            final_nt->OptionalHeader
                .DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};
        }
    }

    if (!write_file(options.output, image, error)) {
        console::failure("%s", error.c_str());
        return 1;
    }

    console::success(
        "saved %s (%zu bytes)",
        text::to_utf8(options.output.wstring()).c_str(),
        image.size());

    if (options.write_report) {
        std::string report_error;
        if (write_json_report(
                options.report,
                pid,
                *module,
                options,
                code_total,
                data_total,
                memory_issues,
                restored_relocations,
                found_imports,
                imports_rebuilt,
                patched_references,
                patched_slots,
                report_error)) {
            console::success(
                "saved report %s",
                text::to_utf8(options.report.wstring()).c_str());
        } else {
            console::warning(
                "report was not written: %s",
                report_error.c_str());
        }
    }

    if (!options.minidump.empty()) {
        std::string minidump_error;
        if (write_minidump(
                process_handle.get(),
                pid,
                options.minidump,
                minidump_error)) {
            console::success(
                "saved minidump %s",
                text::to_utf8(options.minidump.wstring()).c_str());
        } else {
            console::warning(
                "minidump was not written: %s",
                minidump_error.c_str());
        }
    }
    return 0;
}
