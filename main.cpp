#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

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

namespace {

constexpr std::string_view kVersion = "0.2.1";
constexpr SIZE_T kPageSize = 0x1000;

using NtFlushInstructionCacheFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, ULONG);

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
    size_t refreshes,
    size_t failures) {
    const double percentage = total == 0
        ? 100.0
        : 100.0 * static_cast<double>(current) / static_cast<double>(total);
    std::printf(
        "\r  %-8.*s [%5zu/%5zu] %6.2f%%  read=%zu  refresh=%zu  failed=%zu",
        static_cast<int>((std::min)(section.size(), size_t{8})),
        section.data(),
        current,
        total,
        percentage,
        bytes_read,
        refreshes,
        failures);
    std::fflush(stdout);
}

}  // namespace console

struct Options {
    std::string process_name = "RobloxPlayerBeta.exe";
    std::optional<DWORD> pid;
    std::filesystem::path output;
    double code_limit = 1.0;
    bool dump_data = true;
    bool rebuild_imports = true;
    bool aggressive_import_scan = false;
    bool show_help = false;
};

void print_usage() {
    std::printf(
        "Usage:\n"
        "  decrypton.exe [process.exe] [limit-percent]\n"
        "  decrypton.exe --process <name> [options]\n"
        "  decrypton.exe --pid <id> [options]\n\n"
        "Options:\n"
        "  -p, --process <name>       Target process name\n"
        "      --pid <id>             Target process ID\n"
        "  -o, --output <path>        Output PE path\n"
        "  -l, --limit <1-100>        Percentage of each code section to copy\n"
        "      --no-data              Do not refresh non-executable sections\n"
        "      --no-imports           Keep the original import directory\n"
        "      --aggressive-imports   Scan executable sections for resolved imports\n"
        "  -h, --help                 Show this help\n");
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
        } else if (argument == "--no-data") {
            options.dump_data = false;
        } else if (argument == "--no-imports") {
            options.rebuild_imports = false;
        } else if (argument == "--aggressive-imports") {
            options.aggressive_import_scan = true;
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

    if (options.process_name.empty() && !options.pid) {
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

    return first;
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

bool is_readable(HANDLE process, const void* address) {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQueryEx(
            process,
            address,
            &information,
            sizeof(information)) != sizeof(information)) {
        return false;
    }

    if (information.State != MEM_COMMIT ||
        (information.Protect & PAGE_GUARD) != 0 ||
        (information.Protect & PAGE_NOACCESS) != 0 ||
        information.Protect == 0) {
        return false;
    }
    return true;
}

SIZE_T read(HANDLE process, const void* address, void* output, SIZE_T size) {
    SIZE_T bytes_read = 0;
    ReadProcessMemory(process, address, output, size, &bytes_read);
    return bytes_read;
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

struct CopyStats {
    size_t requested = 0;
    size_t copied = 0;
    size_t refreshes = 0;
    size_t failed_pages = 0;
};

CopyStats copy_section_from_process(
    HANDLE process_handle,
    PVOID module_base,
    DWORD module_size,
    const IMAGE_SECTION_HEADER& section,
    double limit,
    NtFlushInstructionCacheFn refresh,
    std::vector<BYTE>& image,
    RvaRanges& copied_ranges) {
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
        if (memory::is_readable(process_handle, remote)) {
            count = memory::read(process_handle, remote, local, chunk);
        }

        if (count == 0 && refresh != nullptr) {
            refresh(
                process_handle,
                remote,
                static_cast<ULONG>(chunk));
            ++stats.refreshes;
            if (memory::is_readable(process_handle, remote)) {
                count = memory::read(process_handle, remote, local, chunk);
            }
        }

        if (count > 0) {
            stats.copied += count;
            copied_ranges.add(
                section.VirtualAddress + static_cast<uint32_t>(offset),
                count);
        }
        if (count < chunk) {
            ++stats.failed_pages;
        }

        console::progress(
            name,
            page + 1,
            selected_pages,
            stats.copied,
            stats.refreshes,
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

        const uint32_t entry_bytes = block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
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
        if (!rva || !seen_rvas.insert(*rva).second) {
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
        const bool allow_single = aggressive || section_name == ".idata" ||
            section_name == ".rdata" || section_name == ".data";

        std::vector<DWORD> run;
        auto commit_run = [&]() {
            if (allow_single || run.size() >= 2) {
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
    const std::unordered_map<uint32_t, uint32_t>& old_iat_to_new_iat) {
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
    std::string& error) {
    patched_references = 0;
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
    std::memset(new_section, 0, sizeof(*new_section));
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

    patched_references = patch_rip_relative_references(image, old_iat_to_new_iat);
    return true;
}

}  // namespace imports

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
    if (stem.empty()) {
        stem = L"decrypton";
    }
    return stem + L"-dumped.exe";
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

    const std::wstring process_name = text::to_wide(options.process_name);
    if (process_name.empty() && !options.pid) {
        console::failure("process name is not valid UTF-8");
        return 2;
    }

    const DWORD pid = options.pid.value_or(process::find(process_name));
    if (pid == 0) {
        console::failure("target process is not running");
        return 1;
    }

    UniqueHandle process_handle(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION,
        FALSE,
        pid));
    if (!process_handle) {
        console::failure("OpenProcess failed: %s", text::win32_error().c_str());
        return 1;
    }

    const auto module = process::main_module(pid, options.pid ? L"" : process_name);
    if (!module) {
        console::failure("main module lookup failed: %s", text::win32_error().c_str());
        return 1;
    }

    if (options.output.empty()) {
        options.output = default_output_name(module->name);
    }

    console::info("PID: %lu\n", pid);
    console::info(
        "module: %s @ %p (%lu bytes)\n",
        text::to_utf8(module->name).c_str(),
        module->base,
        module->size);
    console::info("disk image: %s\n", text::to_utf8(module->path.wstring()).c_str());
    console::info("code limit: %.2f%%\n", options.code_limit * 100.0);
    console::info("output: %s\n", text::to_utf8(options.output.wstring()).c_str());

    std::vector<BYTE> image;
    if (!read_file(module->path, image, error)) {
        console::failure("%s", error.c_str());
        return 1;
    }

    PeImage pe(image);
    if (!pe.validate(error)) {
        console::failure("invalid input PE: %s", error.c_str());
        return 1;
    }
    console::success("loaded %zu bytes from disk", image.size());

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto refresh = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<NtFlushInstructionCacheFn>(
              GetProcAddress(ntdll, "NtFlushInstructionCache"));

    const auto sections = pe.copy_sections();
    RvaRanges copied_ranges;
    CopyStats code_total;
    CopyStats data_total;

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
            refresh,
            image,
            copied_ranges);
        code_total.requested += stats.requested;
        code_total.copied += stats.copied;
        code_total.refreshes += stats.refreshes;
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
                refresh,
                image,
                copied_ranges);
            data_total.requested += stats.requested;
            data_total.copied += stats.copied;
            data_total.refreshes += stats.refreshes;
            data_total.failed_pages += stats.failed_pages;
        }
    }

    console::success(
        "code copied: %zu / %zu bytes (%zu failed pages)",
        code_total.copied,
        code_total.requested,
        code_total.failed_pages);
    if (options.dump_data) {
        console::success(
            "data copied: %zu / %zu bytes (%zu failed pages)",
            data_total.copied,
            data_total.requested,
            data_total.failed_pages);
    }

    const size_t restored_relocations = restore_relocations(
        image,
        reinterpret_cast<uintptr_t>(module->base),
        copied_ranges);
    if (restored_relocations > 0) {
        console::success("restored %zu relocated addresses", restored_relocations);
    } else {
        console::info("no copied relocation entries required normalization\n");
    }

    if (options.rebuild_imports) {
        console::info("building loaded export map\n");
        const auto exports = imports::build_export_map(process_handle.get(), pid);
        console::success("indexed %zu named exports", exports.size());

        console::info(
            "scanning resolved imports (%s mode)\n",
            options.aggressive_import_scan ? "aggressive" : "safe");
        const auto found = imports::find_imports(
            image,
            exports,
            options.aggressive_import_scan);
        console::success("found %zu import slots", found.size());

        if (!found.empty()) {
            std::map<std::string, size_t, std::less<>> module_counts;
            for (const auto& import : found) {
                ++module_counts[import.module];
            }
            for (const auto& [name, count] : module_counts) {
                std::printf("  %-24s %zu\n", name.c_str(), count);
            }

            size_t patched_references = 0;
            if (imports::rebuild(image, found, patched_references, error)) {
                console::success(
                    "rebuilt imports and patched %zu RIP-relative references",
                    patched_references);
            } else {
                console::warning("import rebuild skipped: %s", error.c_str());
            }
        } else {
            console::warning("no imports found; preserving the original directory");
        }
    }

    PeImage final_pe(image);
    if (!final_pe.validate(error)) {
        console::failure("output validation failed: %s", error.c_str());
        return 1;
    }

    IMAGE_NT_HEADERS64* final_nt = final_pe.nt();
    if (final_nt != nullptr) {
        final_nt->OptionalHeader.CheckSum = 0;
        if (final_nt->OptionalHeader.NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_SECURITY) {
            final_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {};
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
    return 0;
}

// ignore this comment
