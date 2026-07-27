#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "decrypton/app.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {

constexpr std::string_view kVersion = "0.4.0";
constexpr size_t kMaximumStringLength = 4096;
constexpr size_t kMaximumImportEntries = 1'000'000;

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

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 16);
    for (const char raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                char buffer[7]{};
                std::snprintf(
                    buffer, sizeof(buffer), "\\u%04x",
                    static_cast<unsigned int>(character));
                output += buffer;
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

}  // namespace text

bool read_file(
    const std::filesystem::path& path,
    std::vector<BYTE>& output,
    std::string& error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "cannot open input file: " + text::to_utf8(path.wstring());
        return false;
    }

    const std::streamoff length = stream.tellg();
    if (length < 0 ||
        static_cast<uint64_t>(length) > (std::numeric_limits<size_t>::max)()) {
        error = "invalid input file size";
        return false;
    }

    output.resize(static_cast<size_t>(length));
    stream.seekg(0, std::ios::beg);
    if (!output.empty()) {
        stream.read(
            reinterpret_cast<char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
    }
    if (!stream && !output.empty()) {
        error = "failed to read the complete input file";
        return false;
    }
    return true;
}

bool write_file_atomic(
    const std::filesystem::path& path,
    const std::vector<BYTE>& bytes,
    std::string& error) {
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "cannot create output directory: " + filesystem_error.message();
            return false;
        }
    }

    std::filesystem::path temporary = path;
    temporary += L".tmp";

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot create temporary output file";
            return false;
        }
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            error = "failed while writing the output file";
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }

    std::filesystem::remove(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        error = "cannot replace output file: " + filesystem_error.message();
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

bool write_text_atomic(
    const std::filesystem::path& path,
    std::string_view content,
    std::string& error) {
    std::vector<BYTE> bytes;
    bytes.reserve(content.size());
    for (const char character : content) {
        bytes.push_back(static_cast<BYTE>(static_cast<unsigned char>(character)));
    }
    return write_file_atomic(path, bytes, error);
}

class BCryptAlgorithm {
public:
    BCryptAlgorithm() = default;
    ~BCryptAlgorithm() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;

    BCRYPT_ALG_HANDLE* put() { return &handle_; }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHash {
public:
    BCryptHash() = default;
    ~BCryptHash() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }
    BCryptHash(const BCryptHash&) = delete;
    BCryptHash& operator=(const BCryptHash&) = delete;

    BCRYPT_HASH_HANDLE* put() { return &handle_; }
    BCRYPT_HASH_HANDLE get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

class BCryptKey {
public:
    BCryptKey() = default;
    ~BCryptKey() {
        if (handle_ != nullptr) {
            BCryptDestroyKey(handle_);
        }
    }
    BCryptKey(const BCryptKey&) = delete;
    BCryptKey& operator=(const BCryptKey&) = delete;

    BCRYPT_KEY_HANDLE* put() { return &handle_; }
    BCRYPT_KEY_HANDLE get() const { return handle_; }

private:
    BCRYPT_KEY_HANDLE handle_ = nullptr;
};

std::string bytes_to_hex(const BYTE* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        output[index * 2] = digits[(data[index] >> 4) & 0x0F];
        output[index * 2 + 1] = digits[data[index] & 0x0F];
    }
    return output;
}

bool sha256(
    const std::vector<BYTE>& bytes,
    std::array<BYTE, 32>& digest,
    std::string& error) {
    BCryptAlgorithm algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptOpenAlgorithmProvider(SHA-256) failed";
        return false;
    }

    DWORD object_size = 0;
    DWORD received = 0;
    status = BCryptGetProperty(
        algorithm.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size),
        &received,
        0);
    if (!BCRYPT_SUCCESS(status) || received != sizeof(object_size)) {
        error = "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
        return false;
    }

    std::vector<BYTE> object(object_size);
    BCryptHash hash;
    status = BCryptCreateHash(
        algorithm.get(),
        hash.put(),
        object.data(),
        static_cast<ULONG>(object.size()),
        nullptr,
        0,
        0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptCreateHash failed";
        return false;
    }

    size_t offset = 0;
    while (offset < bytes.size()) {
        const size_t chunk_size = (std::min)(
            bytes.size() - offset,
            static_cast<size_t>((std::numeric_limits<ULONG>::max)()));
        status = BCryptHashData(
            hash.get(),
            const_cast<PUCHAR>(bytes.data() + offset),
            static_cast<ULONG>(chunk_size),
            0);
        if (!BCRYPT_SUCCESS(status)) {
            error = "BCryptHashData failed";
            return false;
        }
        offset += chunk_size;
    }

    status = BCryptFinishHash(
        hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptFinishHash failed";
        return false;
    }
    return true;
}

double entropy(const BYTE* data, size_t size) {
    if (size == 0) {
        return 0.0;
    }

    std::array<size_t, 256> frequency{};
    for (size_t index = 0; index < size; ++index) {
        ++frequency[data[index]];
    }

    double result = 0.0;
    const double denominator = static_cast<double>(size);
    for (const size_t count : frequency) {
        if (count == 0) {
            continue;
        }
        const double probability = static_cast<double>(count) / denominator;
        result -= probability * std::log2(probability);
    }
    return result;
}

struct SectionInfo {
    std::string name;
    DWORD virtual_address = 0;
    DWORD virtual_size = 0;
    DWORD raw_offset = 0;
    DWORD raw_size = 0;
    DWORD characteristics = 0;
};

struct DirectoryInfo {
    DWORD rva = 0;
    DWORD size = 0;
};

enum class PeKind {
    pe32,
    pe64,
};

class PeView {
public:
    explicit PeView(const std::vector<BYTE>& bytes) : bytes_(bytes) {}

    bool parse(std::string& error) {
        parsed_ = false;
        sections_.clear();
        directories_.fill({});

        if (bytes_.size() < sizeof(IMAGE_DOS_HEADER)) {
            error = "file is smaller than IMAGE_DOS_HEADER";
            return false;
        }

        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, bytes_.data(), sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            error = "invalid DOS header";
            return false;
        }

        nt_offset_ = static_cast<size_t>(dos.e_lfanew);
        if (nt_offset_ > bytes_.size() ||
            bytes_.size() - nt_offset_ < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) {
            error = "truncated NT headers";
            return false;
        }

        DWORD signature = 0;
        std::memcpy(&signature, bytes_.data() + nt_offset_, sizeof(signature));
        if (signature != IMAGE_NT_SIGNATURE) {
            error = "invalid NT signature";
            return false;
        }

        std::memcpy(
            &file_header_,
            bytes_.data() + nt_offset_ + sizeof(DWORD),
            sizeof(file_header_));
        if (file_header_.NumberOfSections == 0 ||
            file_header_.NumberOfSections > 96) {
            error = "invalid section count";
            return false;
        }

        optional_offset_ = nt_offset_ + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (optional_offset_ > bytes_.size() ||
            bytes_.size() - optional_offset_ < file_header_.SizeOfOptionalHeader ||
            file_header_.SizeOfOptionalHeader < sizeof(WORD)) {
            error = "truncated optional header";
            return false;
        }

        WORD magic = 0;
        std::memcpy(&magic, bytes_.data() + optional_offset_, sizeof(magic));
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            if (file_header_.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
                error = "truncated PE32+ optional header";
                return false;
            }
            IMAGE_OPTIONAL_HEADER64 optional{};
            std::memcpy(&optional, bytes_.data() + optional_offset_, sizeof(optional));
            kind_ = PeKind::pe64;
            image_base_ = optional.ImageBase;
            entry_point_ = optional.AddressOfEntryPoint;
            size_of_image_ = optional.SizeOfImage;
            size_of_headers_ = optional.SizeOfHeaders;
            file_alignment_ = optional.FileAlignment;
            section_alignment_ = optional.SectionAlignment;
            subsystem_ = optional.Subsystem;
            dll_characteristics_ = optional.DllCharacteristics;
            checksum_ = optional.CheckSum;
            number_of_directories_ = (std::min)(
                optional.NumberOfRvaAndSizes,
                static_cast<DWORD>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES));
            for (DWORD index = 0; index < number_of_directories_; ++index) {
                directories_[index] = {
                    optional.DataDirectory[index].VirtualAddress,
                    optional.DataDirectory[index].Size,
                };
            }
        } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            if (file_header_.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) {
                error = "truncated PE32 optional header";
                return false;
            }
            IMAGE_OPTIONAL_HEADER32 optional{};
            std::memcpy(&optional, bytes_.data() + optional_offset_, sizeof(optional));
            kind_ = PeKind::pe32;
            image_base_ = optional.ImageBase;
            entry_point_ = optional.AddressOfEntryPoint;
            size_of_image_ = optional.SizeOfImage;
            size_of_headers_ = optional.SizeOfHeaders;
            file_alignment_ = optional.FileAlignment;
            section_alignment_ = optional.SectionAlignment;
            subsystem_ = optional.Subsystem;
            dll_characteristics_ = optional.DllCharacteristics;
            checksum_ = optional.CheckSum;
            number_of_directories_ = (std::min)(
                optional.NumberOfRvaAndSizes,
                static_cast<DWORD>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES));
            for (DWORD index = 0; index < number_of_directories_; ++index) {
                directories_[index] = {
                    optional.DataDirectory[index].VirtualAddress,
                    optional.DataDirectory[index].Size,
                };
            }
        } else {
            error = "unsupported optional-header magic";
            return false;
        }

        const size_t section_offset = optional_offset_ + file_header_.SizeOfOptionalHeader;
        const size_t section_bytes =
            static_cast<size_t>(file_header_.NumberOfSections) *
            sizeof(IMAGE_SECTION_HEADER);
        if (section_offset > bytes_.size() ||
            bytes_.size() - section_offset < section_bytes) {
            error = "truncated section table";
            return false;
        }

        sections_.reserve(file_header_.NumberOfSections);
        for (WORD index = 0; index < file_header_.NumberOfSections; ++index) {
            IMAGE_SECTION_HEADER section{};
            std::memcpy(
                &section,
                bytes_.data() + section_offset +
                    static_cast<size_t>(index) * sizeof(section),
                sizeof(section));
            char name[9]{};
            std::memcpy(name, section.Name, 8);
            sections_.push_back({
                name,
                section.VirtualAddress,
                section.Misc.VirtualSize,
                section.PointerToRawData,
                section.SizeOfRawData,
                section.Characteristics,
            });
        }

        parsed_ = true;
        return true;
    }

    [[nodiscard]] bool parsed() const { return parsed_; }
    [[nodiscard]] PeKind kind() const { return kind_; }
    [[nodiscard]] WORD machine() const { return file_header_.Machine; }
    [[nodiscard]] WORD characteristics() const { return file_header_.Characteristics; }
    [[nodiscard]] WORD subsystem() const { return subsystem_; }
    [[nodiscard]] WORD dll_characteristics() const { return dll_characteristics_; }
    [[nodiscard]] DWORD entry_point() const { return entry_point_; }
    [[nodiscard]] uint64_t image_base() const { return image_base_; }
    [[nodiscard]] DWORD size_of_image() const { return size_of_image_; }
    [[nodiscard]] DWORD size_of_headers() const { return size_of_headers_; }
    [[nodiscard]] DWORD file_alignment() const { return file_alignment_; }
    [[nodiscard]] DWORD section_alignment() const { return section_alignment_; }
    [[nodiscard]] DWORD checksum() const { return checksum_; }
    [[nodiscard]] const std::vector<SectionInfo>& sections() const { return sections_; }
    [[nodiscard]] const std::array<DirectoryInfo, IMAGE_NUMBEROF_DIRECTORY_ENTRIES>&
    directories() const { return directories_; }
    [[nodiscard]] const std::vector<BYTE>& bytes() const { return bytes_; }

    std::optional<size_t> rva_to_offset(DWORD rva, size_t required = 1) const {
        if (!parsed_) {
            return std::nullopt;
        }

        const uint64_t end = static_cast<uint64_t>(rva) + required;
        if (end > (std::numeric_limits<DWORD>::max)()) {
            return std::nullopt;
        }

        if (rva < size_of_headers_) {
            if (end <= bytes_.size()) {
                return static_cast<size_t>(rva);
            }
            return std::nullopt;
        }

        for (const SectionInfo& section : sections_) {
            const uint64_t span = (std::max)(
                static_cast<uint64_t>(section.virtual_size),
                static_cast<uint64_t>(section.raw_size));
            const uint64_t begin = section.virtual_address;
            const uint64_t section_end = begin + span;
            if (rva < begin || end > section_end) {
                continue;
            }

            const uint64_t delta = static_cast<uint64_t>(rva) - begin;
            const uint64_t raw = static_cast<uint64_t>(section.raw_offset) + delta;
            const uint64_t raw_end = raw + required;
            const uint64_t section_raw_end =
                static_cast<uint64_t>(section.raw_offset) + section.raw_size;
            if (raw_end <= bytes_.size() && raw_end <= section_raw_end) {
                return static_cast<size_t>(raw);
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> read_ascii_rva(
        DWORD rva,
        size_t maximum = kMaximumStringLength) const {
        const auto offset = rva_to_offset(rva, 1);
        if (!offset) {
            return std::nullopt;
        }

        std::string output;
        output.reserve((std::min)(maximum, size_t{128}));
        for (size_t index = *offset;
             index < bytes_.size() && output.size() < maximum;
             ++index) {
            const BYTE character = bytes_[index];
            if (character == 0) {
                return output;
            }
            if (character < 0x20 || character > 0x7E) {
                return std::nullopt;
            }
            output.push_back(static_cast<char>(character));
        }
        return std::nullopt;
    }

    bool rva_is_executable(DWORD rva) const {
        for (const SectionInfo& section : sections_) {
            const uint64_t span = (std::max)(
                static_cast<uint64_t>(section.virtual_size),
                static_cast<uint64_t>(section.raw_size));
            if (rva >= section.virtual_address &&
                static_cast<uint64_t>(rva) <
                    static_cast<uint64_t>(section.virtual_address) + span) {
                return (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            }
        }
        return false;
    }

private:
    const std::vector<BYTE>& bytes_;
    bool parsed_ = false;
    PeKind kind_ = PeKind::pe64;
    size_t nt_offset_ = 0;
    size_t optional_offset_ = 0;
    IMAGE_FILE_HEADER file_header_{};
    uint64_t image_base_ = 0;
    DWORD entry_point_ = 0;
    DWORD size_of_image_ = 0;
    DWORD size_of_headers_ = 0;
    DWORD file_alignment_ = 0;
    DWORD section_alignment_ = 0;
    WORD subsystem_ = 0;
    WORD dll_characteristics_ = 0;
    DWORD checksum_ = 0;
    DWORD number_of_directories_ = 0;
    std::vector<SectionInfo> sections_;
    std::array<DirectoryInfo, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> directories_{};
};

const char* machine_name(WORD machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: return "x86";
    case IMAGE_FILE_MACHINE_AMD64: return "x64";
    case IMAGE_FILE_MACHINE_ARM64: return "ARM64";
    default: return "unknown";
    }
}

const char* directory_name(size_t index) {
    static constexpr std::array<const char*, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> names = {
        "export", "import", "resource", "exception",
        "security", "basereloc", "debug", "architecture",
        "globalptr", "tls", "load_config", "bound_import",
        "iat", "delay_import", "clr", "reserved",
    };
    return index < names.size() ? names[index] : "unknown";
}

struct Diagnostic {
    enum class Severity {
        warning,
        error,
    };

    Severity severity = Severity::error;
    std::string code;
    std::string message;
};

void add_error(
    std::vector<Diagnostic>& diagnostics,
    std::string code,
    std::string message) {
    diagnostics.push_back({
        Diagnostic::Severity::error,
        std::move(code),
        std::move(message),
    });
}

void add_warning(
    std::vector<Diagnostic>& diagnostics,
    std::string code,
    std::string message) {
    diagnostics.push_back({
        Diagnostic::Severity::warning,
        std::move(code),
        std::move(message),
    });
}

bool is_power_of_two(DWORD value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool checked_range(uint64_t begin, uint64_t size, uint64_t limit) {
    return begin <= limit && size <= limit - begin;
}

void validate_imports(const PeView& pe, std::vector<Diagnostic>& diagnostics) {
    const DirectoryInfo directory =
        pe.directories()[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.rva == 0 || directory.size == 0) {
        return;
    }

    const auto directory_offset = pe.rva_to_offset(directory.rva, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (!directory_offset) {
        add_error(diagnostics, "import.directory", "import directory is not file-backed");
        return;
    }

    const size_t maximum_descriptors =
        (std::max)(size_t{1}, directory.size / sizeof(IMAGE_IMPORT_DESCRIPTOR));
    bool terminated = false;
    for (size_t descriptor_index = 0;
         descriptor_index < maximum_descriptors;
         ++descriptor_index) {
        const uint64_t descriptor_rva64 =
            static_cast<uint64_t>(directory.rva) +
            descriptor_index * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        if (descriptor_rva64 > (std::numeric_limits<DWORD>::max)()) {
            add_error(diagnostics, "import.overflow", "import descriptor RVA overflow");
            return;
        }

        const auto offset = pe.rva_to_offset(
            static_cast<DWORD>(descriptor_rva64),
            sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!offset) {
            add_error(diagnostics, "import.truncated", "truncated import descriptor array");
            return;
        }

        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, pe.bytes().data() + *offset, sizeof(descriptor));
        if (descriptor.OriginalFirstThunk == 0 && descriptor.FirstThunk == 0 &&
            descriptor.Name == 0 && descriptor.TimeDateStamp == 0 &&
            descriptor.ForwarderChain == 0) {
            terminated = true;
            break;
        }

        const auto module_name = pe.read_ascii_rva(descriptor.Name);
        if (!module_name || module_name->empty()) {
            add_error(
                diagnostics,
                "import.module_name",
                "invalid import module name at descriptor " +
                    std::to_string(descriptor_index));
        }

        const DWORD lookup_rva = descriptor.OriginalFirstThunk != 0
            ? descriptor.OriginalFirstThunk
            : descriptor.FirstThunk;
        if (lookup_rva == 0 || descriptor.FirstThunk == 0) {
            add_error(
                diagnostics,
                "import.thunk",
                "missing import thunk RVA at descriptor " +
                    std::to_string(descriptor_index));
            continue;
        }

        bool thunk_terminated = false;
        const size_t thunk_size = pe.kind() == PeKind::pe64 ? sizeof(uint64_t) : sizeof(uint32_t);
        for (size_t thunk_index = 0; thunk_index < kMaximumImportEntries; ++thunk_index) {
            const uint64_t delta = thunk_index * thunk_size;
            const uint64_t lookup_entry_rva64 = static_cast<uint64_t>(lookup_rva) + delta;
            const uint64_t iat_entry_rva64 = static_cast<uint64_t>(descriptor.FirstThunk) + delta;
            if (lookup_entry_rva64 > (std::numeric_limits<DWORD>::max)() ||
                iat_entry_rva64 > (std::numeric_limits<DWORD>::max)()) {
                add_error(diagnostics, "import.thunk_overflow", "import thunk RVA overflow");
                return;
            }

            const auto lookup_offset = pe.rva_to_offset(
                static_cast<DWORD>(lookup_entry_rva64), thunk_size);
            const auto iat_offset = pe.rva_to_offset(
                static_cast<DWORD>(iat_entry_rva64), thunk_size);
            if (!lookup_offset || !iat_offset) {
                add_error(
                    diagnostics,
                    "import.thunk_bounds",
                    "import thunk is outside file-backed image at descriptor " +
                        std::to_string(descriptor_index));
                break;
            }

            uint64_t value = 0;
            std::memcpy(&value, pe.bytes().data() + *lookup_offset, thunk_size);
            if (value == 0) {
                thunk_terminated = true;
                break;
            }

            const uint64_t ordinal_flag = pe.kind() == PeKind::pe64
                ? IMAGE_ORDINAL_FLAG64
                : IMAGE_ORDINAL_FLAG32;
            if ((value & ordinal_flag) == 0) {
                if (value > (std::numeric_limits<DWORD>::max)()) {
                    add_error(diagnostics, "import.name_overflow", "import-by-name RVA overflow");
                    break;
                }
                const DWORD name_rva = static_cast<DWORD>(value);
                if (name_rva > (std::numeric_limits<DWORD>::max)() - sizeof(WORD)) {
                    add_error(diagnostics, "import.name_overflow", "import name RVA overflow");
                    break;
                }
                const auto hint_offset = pe.rva_to_offset(name_rva, sizeof(WORD));
                const auto function_name = pe.read_ascii_rva(
                    name_rva + static_cast<DWORD>(sizeof(WORD)));
                if (!hint_offset || !function_name || function_name->empty()) {
                    add_error(
                        diagnostics,
                        "import.function_name",
                        "invalid import-by-name entry at descriptor " +
                            std::to_string(descriptor_index));
                    break;
                }
            }
        }

        if (!thunk_terminated) {
            add_error(
                diagnostics,
                "import.thunk_termination",
                "unterminated or excessively large import thunk array at descriptor " +
                    std::to_string(descriptor_index));
        }
    }

    if (!terminated) {
        add_error(diagnostics, "import.termination", "import descriptor array is not terminated");
    }
}

void validate_relocations(const PeView& pe, std::vector<Diagnostic>& diagnostics) {
    const DirectoryInfo directory =
        pe.directories()[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.rva == 0 || directory.size == 0) {
        return;
    }

    uint32_t consumed = 0;
    while (consumed < directory.size) {
        if (directory.size - consumed < sizeof(IMAGE_BASE_RELOCATION)) {
            add_error(diagnostics, "reloc.truncated", "truncated relocation block header");
            return;
        }

        const uint64_t block_rva64 = static_cast<uint64_t>(directory.rva) + consumed;
        if (block_rva64 > (std::numeric_limits<DWORD>::max)()) {
            add_error(diagnostics, "reloc.overflow", "relocation block RVA overflow");
            return;
        }

        const auto offset = pe.rva_to_offset(
            static_cast<DWORD>(block_rva64), sizeof(IMAGE_BASE_RELOCATION));
        if (!offset) {
            add_error(diagnostics, "reloc.bounds", "relocation block is not file-backed");
            return;
        }

        IMAGE_BASE_RELOCATION block{};
        std::memcpy(&block, pe.bytes().data() + *offset, sizeof(block));
        if (block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block.SizeOfBlock > directory.size - consumed ||
            ((block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(WORD)) != 0) {
            add_error(diagnostics, "reloc.block_size", "invalid relocation block size");
            return;
        }

        if (!pe.rva_to_offset(static_cast<DWORD>(block_rva64), block.SizeOfBlock)) {
            add_error(diagnostics, "reloc.block_bounds", "relocation block exceeds file-backed data");
            return;
        }
        consumed += block.SizeOfBlock;
    }
}

void validate_exports(const PeView& pe, std::vector<Diagnostic>& diagnostics) {
    const DirectoryInfo directory =
        pe.directories()[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.rva == 0 || directory.size == 0) {
        return;
    }

    const auto offset = pe.rva_to_offset(directory.rva, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!offset) {
        add_error(diagnostics, "export.directory", "export directory is not file-backed");
        return;
    }

    IMAGE_EXPORT_DIRECTORY exports{};
    std::memcpy(&exports, pe.bytes().data() + *offset, sizeof(exports));
    if (exports.Name != 0 && !pe.read_ascii_rva(exports.Name)) {
        add_error(diagnostics, "export.name", "invalid export module name");
    }

    if (exports.NumberOfFunctions > kMaximumImportEntries ||
        exports.NumberOfNames > kMaximumImportEntries ||
        exports.NumberOfNames > exports.NumberOfFunctions) {
        add_error(diagnostics, "export.count", "invalid export counts");
        return;
    }

    if (exports.NumberOfFunctions > 0 &&
        !pe.rva_to_offset(
            exports.AddressOfFunctions,
            static_cast<size_t>(exports.NumberOfFunctions) * sizeof(DWORD))) {
        add_error(diagnostics, "export.functions", "invalid export address table");
    }
    if (exports.NumberOfNames > 0) {
        if (!pe.rva_to_offset(
                exports.AddressOfNames,
                static_cast<size_t>(exports.NumberOfNames) * sizeof(DWORD))) {
            add_error(diagnostics, "export.names", "invalid export name-pointer table");
        }
        if (!pe.rva_to_offset(
                exports.AddressOfNameOrdinals,
                static_cast<size_t>(exports.NumberOfNames) * sizeof(WORD))) {
            add_error(diagnostics, "export.ordinals", "invalid export ordinal table");
        }
    }
}

std::vector<Diagnostic> verify_pe(const PeView& pe) {
    std::vector<Diagnostic> diagnostics;
    const auto& bytes = pe.bytes();

    if (pe.machine() == IMAGE_FILE_MACHINE_AMD64 && pe.kind() != PeKind::pe64) {
        add_error(diagnostics, "machine.magic", "AMD64 image does not use PE32+");
    }
    if (pe.machine() == IMAGE_FILE_MACHINE_I386 && pe.kind() != PeKind::pe32) {
        add_error(diagnostics, "machine.magic", "I386 image does not use PE32");
    }

    if (!is_power_of_two(pe.file_alignment()) ||
        pe.file_alignment() < 0x200 || pe.file_alignment() > 0x10000) {
        add_error(diagnostics, "alignment.file", "invalid FileAlignment");
    }
    if (!is_power_of_two(pe.section_alignment()) || pe.section_alignment() == 0) {
        add_error(diagnostics, "alignment.section", "invalid SectionAlignment");
    }
    if (pe.section_alignment() < 0x1000 &&
        pe.section_alignment() != pe.file_alignment()) {
        add_error(
            diagnostics,
            "alignment.low",
            "SectionAlignment below page size must equal FileAlignment");
    } else if (pe.section_alignment() < pe.file_alignment()) {
        add_error(
            diagnostics,
            "alignment.order",
            "SectionAlignment is smaller than FileAlignment");
    }

    if (pe.size_of_headers() == 0 || pe.size_of_headers() > bytes.size()) {
        add_error(diagnostics, "headers.size", "SizeOfHeaders is outside the file");
    } else if (pe.file_alignment() != 0 &&
               (pe.size_of_headers() % pe.file_alignment()) != 0) {
        add_warning(diagnostics, "headers.alignment", "SizeOfHeaders is not file-aligned");
    }

    struct Range {
        uint64_t begin = 0;
        uint64_t end = 0;
        std::string name;
    };
    std::vector<Range> raw_ranges;
    std::vector<Range> virtual_ranges;
    uint64_t highest_virtual_end = pe.size_of_headers();

    for (const SectionInfo& section : pe.sections()) {
        if (section.raw_size > 0) {
            if (!checked_range(section.raw_offset, section.raw_size, bytes.size())) {
                add_error(
                    diagnostics,
                    "section.raw_bounds",
                    "section '" + section.name + "' exceeds the file");
            } else {
                raw_ranges.push_back({
                    section.raw_offset,
                    static_cast<uint64_t>(section.raw_offset) + section.raw_size,
                    section.name,
                });
            }
            if (pe.file_alignment() != 0 &&
                (section.raw_offset % pe.file_alignment()) != 0) {
                add_warning(
                    diagnostics,
                    "section.raw_alignment",
                    "section '" + section.name + "' raw offset is not aligned");
            }
        }

        const uint64_t virtual_span = (std::max)(
            static_cast<uint64_t>(section.virtual_size),
            static_cast<uint64_t>(section.raw_size));
        if (virtual_span > 0) {
            const uint64_t virtual_end =
                static_cast<uint64_t>(section.virtual_address) + virtual_span;
            if (virtual_end > (std::numeric_limits<DWORD>::max)()) {
                add_error(
                    diagnostics,
                    "section.virtual_overflow",
                    "section '" + section.name + "' virtual range overflows");
            } else {
                virtual_ranges.push_back({
                    section.virtual_address,
                    virtual_end,
                    section.name,
                });
                highest_virtual_end = (std::max)(highest_virtual_end, virtual_end);
            }
            if (pe.section_alignment() != 0 &&
                (section.virtual_address % pe.section_alignment()) != 0) {
                add_warning(
                    diagnostics,
                    "section.virtual_alignment",
                    "section '" + section.name + "' RVA is not aligned");
            }
        }
    }

    auto check_overlap = [&](std::vector<Range> ranges, std::string_view code) {
        std::sort(ranges.begin(), ranges.end(), [](const Range& lhs, const Range& rhs) {
            return lhs.begin < rhs.begin;
        });
        for (size_t index = 1; index < ranges.size(); ++index) {
            if (ranges[index].begin < ranges[index - 1].end) {
                add_error(
                    diagnostics,
                    std::string(code),
                    "sections '" + ranges[index - 1].name + "' and '" +
                        ranges[index].name + "' overlap");
            }
        }
    };
    check_overlap(raw_ranges, "section.raw_overlap");
    check_overlap(virtual_ranges, "section.virtual_overlap");

    if (pe.size_of_image() < highest_virtual_end) {
        add_error(diagnostics, "image.size", "SizeOfImage does not cover all sections");
    } else if (pe.section_alignment() != 0 &&
               (pe.size_of_image() % pe.section_alignment()) != 0) {
        add_warning(diagnostics, "image.alignment", "SizeOfImage is not section-aligned");
    }

    if (pe.entry_point() != 0) {
        if (!pe.rva_to_offset(pe.entry_point(), 1)) {
            add_error(diagnostics, "entry.bounds", "entry point is not file-backed");
        } else if (!pe.rva_is_executable(pe.entry_point())) {
            add_warning(diagnostics, "entry.executable", "entry point is not in an executable section");
        }
    }

    for (size_t index = 0; index < pe.directories().size(); ++index) {
        const DirectoryInfo directory = pe.directories()[index];
        if (directory.rva == 0 || directory.size == 0) {
            continue;
        }

        if (index == IMAGE_DIRECTORY_ENTRY_SECURITY) {
            if (!checked_range(directory.rva, directory.size, bytes.size())) {
                add_error(
                    diagnostics,
                    "directory.security",
                    "security directory exceeds the file");
            }
            continue;
        }

        if (!pe.rva_to_offset(directory.rva, directory.size)) {
            add_error(
                diagnostics,
                "directory.bounds",
                std::string(directory_name(index)) +
                    " directory is not fully file-backed");
        }
    }

    const DirectoryInfo exception_directory =
        pe.directories()[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (pe.kind() == PeKind::pe64 && exception_directory.size != 0 &&
        (exception_directory.size % sizeof(RUNTIME_FUNCTION)) != 0) {
        add_error(
            diagnostics,
            "exception.size",
            "x64 exception directory size is not a multiple of RUNTIME_FUNCTION");
    }

    const DirectoryInfo tls_directory = pe.directories()[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_directory.size != 0) {
        const size_t required = pe.kind() == PeKind::pe64
            ? sizeof(IMAGE_TLS_DIRECTORY64)
            : sizeof(IMAGE_TLS_DIRECTORY32);
        if (tls_directory.size < required ||
            !pe.rva_to_offset(tls_directory.rva, required)) {
            add_error(diagnostics, "tls.directory", "invalid TLS directory");
        }
    }

    validate_imports(pe, diagnostics);
    validate_relocations(pe, diagnostics);
    validate_exports(pe, diagnostics);
    return diagnostics;
}

std::string inspect_json(
    const std::filesystem::path& path,
    const PeView& pe,
    std::string_view hash) {
    std::ostringstream output;
    output << "{\n"
           << "  \"tool\": \"decrypton\",\n"
           << "  \"version\": \"" << kVersion << "\",\n"
           << "  \"command\": \"inspect\",\n"
           << "  \"path\": \"" << text::json_escape(text::to_utf8(path.wstring())) << "\",\n"
           << "  \"sha256\": \"" << hash << "\",\n"
           << "  \"size\": " << pe.bytes().size() << ",\n"
           << "  \"format\": \"" << (pe.kind() == PeKind::pe64 ? "PE32+" : "PE32") << "\",\n"
           << "  \"machine\": \"" << machine_name(pe.machine()) << "\",\n"
           << "  \"image_base\": \"0x" << std::hex << pe.image_base() << std::dec << "\",\n"
           << "  \"entry_point_rva\": \"0x" << std::hex << pe.entry_point() << std::dec << "\",\n"
           << "  \"size_of_image\": " << pe.size_of_image() << ",\n"
           << "  \"sections\": [\n";

    for (size_t index = 0; index < pe.sections().size(); ++index) {
        const SectionInfo& section = pe.sections()[index];
        double section_entropy = 0.0;
        if (checked_range(section.raw_offset, section.raw_size, pe.bytes().size())) {
            section_entropy = entropy(
                pe.bytes().data() + section.raw_offset,
                section.raw_size);
        }
        output << "    {\"name\":\"" << text::json_escape(section.name)
               << "\",\"rva\":\"0x" << std::hex << section.virtual_address
               << "\",\"virtual_size\":" << std::dec << section.virtual_size
               << ",\"raw_offset\":" << section.raw_offset
               << ",\"raw_size\":" << section.raw_size
               << ",\"characteristics\":\"0x" << std::hex << section.characteristics
               << "\",\"entropy\":" << std::fixed << std::setprecision(6)
               << section_entropy << std::dec << "}";
        if (index + 1 != pe.sections().size()) {
            output << ',';
        }
        output << '\n';
    }

    output << "  ],\n  \"directories\": [\n";
    bool first = true;
    for (size_t index = 0; index < pe.directories().size(); ++index) {
        const DirectoryInfo directory = pe.directories()[index];
        if (directory.rva == 0 && directory.size == 0) {
            continue;
        }
        if (!first) {
            output << ",\n";
        }
        first = false;
        output << "    {\"name\":\"" << directory_name(index)
               << "\",\"rva\":\"0x" << std::hex << directory.rva
               << "\",\"size\":" << std::dec << directory.size << "}";
    }
    if (!first) {
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::string verify_json(
    const std::filesystem::path& path,
    const std::vector<Diagnostic>& diagnostics,
    bool strict) {
    size_t error_count = 0;
    size_t warning_count = 0;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::error) {
            ++error_count;
        } else {
            ++warning_count;
        }
    }

    const bool valid = error_count == 0 && (!strict || warning_count == 0);
    std::ostringstream output;
    output << "{\n"
           << "  \"tool\": \"decrypton\",\n"
           << "  \"version\": \"" << kVersion << "\",\n"
           << "  \"command\": \"verify\",\n"
           << "  \"path\": \"" << text::json_escape(text::to_utf8(path.wstring())) << "\",\n"
           << "  \"strict\": " << (strict ? "true" : "false") << ",\n"
           << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
           << "  \"errors\": " << error_count << ",\n"
           << "  \"warnings\": " << warning_count << ",\n"
           << "  \"diagnostics\": [\n";
    for (size_t index = 0; index < diagnostics.size(); ++index) {
        const Diagnostic& diagnostic = diagnostics[index];
        output << "    {\"severity\":\""
               << (diagnostic.severity == Diagnostic::Severity::error ? "error" : "warning")
               << "\",\"code\":\"" << text::json_escape(diagnostic.code)
               << "\",\"message\":\"" << text::json_escape(diagnostic.message)
               << "\"}";
        if (index + 1 != diagnostics.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

struct DiffSection {
    std::string name;
    size_t original_size = 0;
    size_t candidate_size = 0;
    size_t changed_bytes = 0;
};

struct DiffResult {
    size_t original_size = 0;
    size_t candidate_size = 0;
    size_t common_size = 0;
    size_t changed_bytes = 0;
    size_t changed_runs = 0;
    size_t changed_blocks = 0;
    size_t block_size = 4096;
    std::string original_sha256;
    std::string candidate_sha256;
    std::vector<DiffSection> sections;
};

DiffResult compare_files(
    const std::vector<BYTE>& original,
    const std::vector<BYTE>& candidate,
    const PeView* original_pe,
    const PeView* candidate_pe,
    size_t block_size) {
    DiffResult result;
    result.original_size = original.size();
    result.candidate_size = candidate.size();
    result.common_size = (std::min)(original.size(), candidate.size());
    result.block_size = block_size;

    bool in_run = false;
    std::set<size_t> changed_blocks;
    for (size_t index = 0; index < result.common_size; ++index) {
        const bool changed = original[index] != candidate[index];
        if (changed) {
            ++result.changed_bytes;
            changed_blocks.insert(index / block_size);
            if (!in_run) {
                ++result.changed_runs;
                in_run = true;
            }
        } else {
            in_run = false;
        }
    }

    if (original.size() != candidate.size()) {
        const size_t tail = original.size() > candidate.size()
            ? original.size() - candidate.size()
            : candidate.size() - original.size();
        result.changed_bytes += tail;
        if (!in_run && tail > 0) {
            ++result.changed_runs;
        }
        const size_t maximum_size = (std::max)(original.size(), candidate.size());
        for (size_t offset = result.common_size; offset < maximum_size; offset += block_size) {
            changed_blocks.insert(offset / block_size);
        }
    }
    result.changed_blocks = changed_blocks.size();

    if (original_pe != nullptr && candidate_pe != nullptr) {
        std::map<std::string, const SectionInfo*, std::less<>> candidate_sections;
        for (const SectionInfo& section : candidate_pe->sections()) {
            candidate_sections.try_emplace(section.name, &section);
        }

        for (const SectionInfo& left : original_pe->sections()) {
            DiffSection section_result;
            section_result.name = left.name;
            section_result.original_size = left.raw_size;
            const auto right_iterator = candidate_sections.find(left.name);
            if (right_iterator == candidate_sections.end()) {
                section_result.changed_bytes = left.raw_size;
                result.sections.push_back(std::move(section_result));
                continue;
            }

            const SectionInfo& right = *right_iterator->second;
            section_result.candidate_size = right.raw_size;
            const size_t left_available = checked_range(
                left.raw_offset, left.raw_size, original.size())
                ? left.raw_size
                : 0;
            const size_t right_available = checked_range(
                right.raw_offset, right.raw_size, candidate.size())
                ? right.raw_size
                : 0;
            const size_t common = (std::min)(left_available, right_available);
            for (size_t index = 0; index < common; ++index) {
                if (original[left.raw_offset + index] !=
                    candidate[right.raw_offset + index]) {
                    ++section_result.changed_bytes;
                }
            }
            section_result.changed_bytes +=
                left_available > right_available
                    ? left_available - right_available
                    : right_available - left_available;
            result.sections.push_back(std::move(section_result));
        }
    }
    return result;
}

std::string diff_json(
    const std::filesystem::path& original_path,
    const std::filesystem::path& candidate_path,
    const DiffResult& result) {
    std::ostringstream output;
    output << "{\n"
           << "  \"tool\": \"decrypton\",\n"
           << "  \"version\": \"" << kVersion << "\",\n"
           << "  \"command\": \"diff\",\n"
           << "  \"original\": {\"path\":\""
           << text::json_escape(text::to_utf8(original_path.wstring()))
           << "\",\"size\":" << result.original_size
           << ",\"sha256\":\"" << result.original_sha256 << "\"},\n"
           << "  \"candidate\": {\"path\":\""
           << text::json_escape(text::to_utf8(candidate_path.wstring()))
           << "\",\"size\":" << result.candidate_size
           << ",\"sha256\":\"" << result.candidate_sha256 << "\"},\n"
           << "  \"changed_bytes\": " << result.changed_bytes << ",\n"
           << "  \"changed_runs\": " << result.changed_runs << ",\n"
           << "  \"block_size\": " << result.block_size << ",\n"
           << "  \"changed_blocks\": " << result.changed_blocks << ",\n"
           << "  \"sections\": [\n";
    for (size_t index = 0; index < result.sections.size(); ++index) {
        const DiffSection& section = result.sections[index];
        output << "    {\"name\":\"" << text::json_escape(section.name)
               << "\",\"original_size\":" << section.original_size
               << ",\"candidate_size\":" << section.candidate_size
               << ",\"changed_bytes\":" << section.changed_bytes << "}";
        if (index + 1 != result.sections.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::optional<std::vector<BYTE>> parse_hex(
    std::string_view value,
    std::string& error) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        if (character == ' ' || character == ':' || character == '-') {
            continue;
        }
        normalized.push_back(character);
    }

    if (normalized.empty() || (normalized.size() % 2) != 0) {
        error = "hex input must contain a non-empty even number of digits";
        return std::nullopt;
    }

    std::vector<BYTE> output(normalized.size() / 2);
    for (size_t index = 0; index < output.size(); ++index) {
        const char pair[] = {
            normalized[index * 2], normalized[index * 2 + 1], '\0'};
        unsigned int byte = 0;
        const auto result = std::from_chars(pair, pair + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != pair + 2 || byte > 0xFF) {
            error = "invalid hexadecimal input";
            return std::nullopt;
        }
        output[index] = static_cast<BYTE>(byte);
    }
    return output;
}

bool aes_cbc_transform(
    const std::vector<BYTE>& input,
    const std::vector<BYTE>& key_bytes,
    const std::vector<BYTE>& iv_bytes,
    bool encrypt,
    std::vector<BYTE>& output,
    std::string& error) {
    if (key_bytes.size() != 32) {
        error = "AES-256 requires a 32-byte key";
        return false;
    }
    if (iv_bytes.size() != 16) {
        error = "AES-CBC requires a 16-byte IV";
        return false;
    }
    if (input.size() > (std::numeric_limits<ULONG>::max)()) {
        error = "input is too large for a single BCrypt operation";
        return false;
    }

    BCryptAlgorithm algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm.put(), BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptOpenAlgorithmProvider(AES) failed";
        return false;
    }

    status = BCryptSetProperty(
        algorithm.get(),
        BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
        static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)),
        0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptSetProperty(CBC) failed";
        return false;
    }

    DWORD object_size = 0;
    DWORD received = 0;
    status = BCryptGetProperty(
        algorithm.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size),
        &received,
        0);
    if (!BCRYPT_SUCCESS(status) || received != sizeof(object_size)) {
        error = "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
        return false;
    }

    std::vector<BYTE> key_object(object_size);
    BCryptKey key;
    status = BCryptGenerateSymmetricKey(
        algorithm.get(),
        key.put(),
        key_object.data(),
        static_cast<ULONG>(key_object.size()),
        const_cast<PUCHAR>(key_bytes.data()),
        static_cast<ULONG>(key_bytes.size()),
        0);
    if (!BCRYPT_SUCCESS(status)) {
        error = "BCryptGenerateSymmetricKey failed";
        return false;
    }

    std::vector<BYTE> iv = iv_bytes;
    ULONG required = 0;
    if (encrypt) {
        status = BCryptEncrypt(
            key.get(),
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            nullptr,
            iv.data(),
            static_cast<ULONG>(iv.size()),
            nullptr,
            0,
            &required,
            BCRYPT_BLOCK_PADDING);
    } else {
        status = BCryptDecrypt(
            key.get(),
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            nullptr,
            iv.data(),
            static_cast<ULONG>(iv.size()),
            nullptr,
            0,
            &required,
            BCRYPT_BLOCK_PADDING);
    }
    if (!BCRYPT_SUCCESS(status)) {
        error = encrypt
            ? "BCryptEncrypt size query failed"
            : "BCryptDecrypt size query failed; verify key, IV and padding";
        return false;
    }

    output.resize(required);
    iv = iv_bytes;
    ULONG written = 0;
    if (encrypt) {
        status = BCryptEncrypt(
            key.get(),
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            nullptr,
            iv.data(),
            static_cast<ULONG>(iv.size()),
            output.data(),
            static_cast<ULONG>(output.size()),
            &written,
            BCRYPT_BLOCK_PADDING);
    } else {
        status = BCryptDecrypt(
            key.get(),
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            nullptr,
            iv.data(),
            static_cast<ULONG>(iv.size()),
            output.data(),
            static_cast<ULONG>(output.size()),
            &written,
            BCRYPT_BLOCK_PADDING);
    }
    if (!BCRYPT_SUCCESS(status)) {
        error = encrypt
            ? "BCryptEncrypt failed"
            : "BCryptDecrypt failed; verify key, IV and padding";
        output.clear();
        return false;
    }
    output.resize(written);
    return true;
}

std::optional<size_t> parse_size(std::string_view value) {
    size_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

void print_offline_banner(std::string_view command) {
    std::printf(
        "decrypton %.*s | %.*s\n\n",
        static_cast<int>(kVersion.size()),
        kVersion.data(),
        static_cast<int>(command.size()),
        command.data());
}

}  // namespace

int run_inspect(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        std::printf(
            "Usage: decrypton inspect <file> [--json <report>]\n"
            "Print PE metadata, directories, section entropy and SHA-256.\n");
        return argc < 2 ? 2 : 0;
    }

    const std::filesystem::path path = text::to_wide(argv[1]);
    std::filesystem::path json_path;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--json" && index + 1 < argc) {
            json_path = text::to_wide(argv[++index]);
        } else {
            std::fprintf(stderr, "unknown inspect option: %s\n", argv[index]);
            return 2;
        }
    }

    print_offline_banner("inspect");
    std::string error;
    std::vector<BYTE> bytes;
    if (!read_file(path, bytes, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }

    PeView pe(bytes);
    if (!pe.parse(error)) {
        std::fprintf(stderr, "[-] invalid PE: %s\n", error.c_str());
        return 1;
    }

    std::array<BYTE, 32> digest{};
    if (!sha256(bytes, digest, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }
    const std::string hash = bytes_to_hex(digest.data(), digest.size());

    std::printf("file: %s\n", text::to_utf8(path.wstring()).c_str());
    std::printf("size: %zu bytes\n", bytes.size());
    std::printf("sha256: %s\n", hash.c_str());
    std::printf("format: %s %s\n",
        pe.kind() == PeKind::pe64 ? "PE32+" : "PE32",
        machine_name(pe.machine()));
    std::printf("image base: 0x%llx\n", static_cast<unsigned long long>(pe.image_base()));
    std::printf(
        "entry point RVA: 0x%08lx\n",
        static_cast<unsigned long>(pe.entry_point()));
    std::printf(
        "size of image: %lu\n",
        static_cast<unsigned long>(pe.size_of_image()));
    std::printf(
        "file/section alignment: 0x%lx / 0x%lx\n\n",
        static_cast<unsigned long>(pe.file_alignment()),
        static_cast<unsigned long>(pe.section_alignment()));

    std::printf("sections:\n");
    for (const SectionInfo& section : pe.sections()) {
        double value = 0.0;
        if (checked_range(section.raw_offset, section.raw_size, bytes.size())) {
            value = entropy(bytes.data() + section.raw_offset, section.raw_size);
        }
        std::printf(
            "  %-8s rva=0x%08lx vsize=%-9lu raw=0x%08lx size=%-9lu entropy=%.4f chars=0x%08lx\n",
            section.name.c_str(),
            static_cast<unsigned long>(section.virtual_address),
            static_cast<unsigned long>(section.virtual_size),
            static_cast<unsigned long>(section.raw_offset),
            static_cast<unsigned long>(section.raw_size),
            value,
            static_cast<unsigned long>(section.characteristics));
    }

    std::printf("\ndirectories:\n");
    for (size_t index = 0; index < pe.directories().size(); ++index) {
        const DirectoryInfo directory = pe.directories()[index];
        if (directory.rva == 0 && directory.size == 0) {
            continue;
        }
        std::printf(
            "  %-14s rva/offset=0x%08lx size=%lu\n",
            directory_name(index),
            static_cast<unsigned long>(directory.rva),
            static_cast<unsigned long>(directory.size));
    }

    if (!json_path.empty()) {
        const std::string json = inspect_json(path, pe, hash);
        if (!write_text_atomic(json_path, json, error)) {
            std::fprintf(stderr, "[-] report: %s\n", error.c_str());
            return 1;
        }
        std::printf("\n[+] saved %s\n", text::to_utf8(json_path.wstring()).c_str());
    }
    return 0;
}

int run_verify(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        std::printf(
            "Usage: decrypton verify <file> [--strict] [--json <report>]\n"
            "Validate headers, sections, data directories, imports, exports and relocations.\n");
        return argc < 2 ? 2 : 0;
    }

    const std::filesystem::path path = text::to_wide(argv[1]);
    std::filesystem::path json_path;
    bool strict = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--strict") {
            strict = true;
        } else if (argument == "--json" && index + 1 < argc) {
            json_path = text::to_wide(argv[++index]);
        } else {
            std::fprintf(stderr, "unknown verify option: %s\n", argv[index]);
            return 2;
        }
    }

    print_offline_banner("verify");
    std::string error;
    std::vector<BYTE> bytes;
    if (!read_file(path, bytes, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }

    PeView pe(bytes);
    if (!pe.parse(error)) {
        std::fprintf(stderr, "[-] invalid PE: %s\n", error.c_str());
        return 1;
    }

    const std::vector<Diagnostic> diagnostics = verify_pe(pe);
    size_t errors = 0;
    size_t warnings = 0;
    for (const Diagnostic& diagnostic : diagnostics) {
        const bool is_error = diagnostic.severity == Diagnostic::Severity::error;
        errors += is_error ? 1 : 0;
        warnings += is_error ? 0 : 1;
        std::printf(
            "%s %-26s %s\n",
            is_error ? "[-]" : "[!]",
            diagnostic.code.c_str(),
            diagnostic.message.c_str());
    }

    if (diagnostics.empty()) {
        std::printf("[+] no structural issues detected\n");
    }
    std::printf("\nsummary: %zu error(s), %zu warning(s)\n", errors, warnings);

    if (!json_path.empty()) {
        const std::string json = verify_json(path, diagnostics, strict);
        if (!write_text_atomic(json_path, json, error)) {
            std::fprintf(stderr, "[-] report: %s\n", error.c_str());
            return 1;
        }
        std::printf("[+] saved %s\n", text::to_utf8(json_path.wstring()).c_str());
    }

    return errors == 0 && (!strict || warnings == 0) ? 0 : 1;
}

int run_diff(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--help") {
        std::printf(
            "Usage: decrypton diff <original> <candidate> [--json <report>] [--block <bytes>]\n"
            "Compare hashes, bytes, changed runs, blocks and matching PE sections.\n");
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "diff requires an original and candidate file\n");
        return 2;
    }

    const std::filesystem::path original_path = text::to_wide(argv[1]);
    const std::filesystem::path candidate_path = text::to_wide(argv[2]);
    std::filesystem::path json_path;
    size_t block_size = 4096;
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--json" && index + 1 < argc) {
            json_path = text::to_wide(argv[++index]);
        } else if (argument == "--block" && index + 1 < argc) {
            const auto parsed = parse_size(argv[++index]);
            if (!parsed || *parsed > 16 * 1024 * 1024) {
                std::fprintf(stderr, "invalid block size\n");
                return 2;
            }
            block_size = *parsed;
        } else {
            std::fprintf(stderr, "unknown diff option: %s\n", argv[index]);
            return 2;
        }
    }

    print_offline_banner("diff");
    std::string error;
    std::vector<BYTE> original;
    std::vector<BYTE> candidate;
    if (!read_file(original_path, original, error) ||
        !read_file(candidate_path, candidate, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }

    PeView original_pe(original);
    PeView candidate_pe(candidate);
    std::string original_parse_error;
    std::string candidate_parse_error;
    const bool original_is_pe = original_pe.parse(original_parse_error);
    const bool candidate_is_pe = candidate_pe.parse(candidate_parse_error);

    DiffResult result = compare_files(
        original,
        candidate,
        original_is_pe ? &original_pe : nullptr,
        candidate_is_pe ? &candidate_pe : nullptr,
        block_size);

    std::array<BYTE, 32> digest{};
    if (!sha256(original, digest, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }
    result.original_sha256 = bytes_to_hex(digest.data(), digest.size());
    if (!sha256(candidate, digest, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }
    result.candidate_sha256 = bytes_to_hex(digest.data(), digest.size());

    const size_t maximum_size = (std::max)(result.original_size, result.candidate_size);
    const double changed_ratio = maximum_size == 0
        ? 0.0
        : static_cast<double>(result.changed_bytes) /
            static_cast<double>(maximum_size);

    std::printf("original:  %zu bytes  %s\n", result.original_size, result.original_sha256.c_str());
    std::printf("candidate: %zu bytes  %s\n", result.candidate_size, result.candidate_sha256.c_str());
    std::printf("changed:   %zu bytes (%.4f%%), %zu runs, %zu blocks of %zu bytes\n",
        result.changed_bytes,
        changed_ratio * 100.0,
        result.changed_runs,
        result.changed_blocks,
        result.block_size);

    if (!result.sections.empty()) {
        std::printf("\nsection differences:\n");
        for (const DiffSection& section : result.sections) {
            std::printf(
                "  %-8s original=%-9zu candidate=%-9zu changed=%zu\n",
                section.name.c_str(),
                section.original_size,
                section.candidate_size,
                section.changed_bytes);
        }
    }

    if (!original_is_pe) {
        std::printf("[!] original is not a parseable PE: %s\n", original_parse_error.c_str());
    }
    if (!candidate_is_pe) {
        std::printf("[!] candidate is not a parseable PE: %s\n", candidate_parse_error.c_str());
    }

    if (!json_path.empty()) {
        const std::string json = diff_json(original_path, candidate_path, result);
        if (!write_text_atomic(json_path, json, error)) {
            std::fprintf(stderr, "[-] report: %s\n", error.c_str());
            return 1;
        }
        std::printf("[+] saved %s\n", text::to_utf8(json_path.wstring()).c_str());
    }
    return result.changed_bytes == 0 ? 0 : 1;
}

int run_transform(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        std::printf(
            "Usage:\n"
            "  decrypton transform <input> --algorithm xor --key-hex <hex> --output <file>\n"
            "  decrypton transform <input> --algorithm aes-256-cbc --mode encrypt|decrypt\n"
            "      --key-hex <64 hex digits> --iv-hex <32 hex digits> --output <file>\n\n"
            "Transform operates only on an offline file with an explicitly supplied key.\n");
        return argc < 2 ? 2 : 0;
    }

    const std::filesystem::path input_path = text::to_wide(argv[1]);
    std::filesystem::path output_path;
    std::string algorithm;
    std::string mode;
    std::string key_hex;
    std::string iv_hex;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto require_value = [&](const char* option) -> const char* {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", option);
                return nullptr;
            }
            return argv[++index];
        };

        if (argument == "--algorithm") {
            const char* value = require_value("--algorithm");
            if (value == nullptr) return 2;
            algorithm = value;
        } else if (argument == "--mode") {
            const char* value = require_value("--mode");
            if (value == nullptr) return 2;
            mode = value;
        } else if (argument == "--key-hex") {
            const char* value = require_value("--key-hex");
            if (value == nullptr) return 2;
            key_hex = value;
        } else if (argument == "--iv-hex") {
            const char* value = require_value("--iv-hex");
            if (value == nullptr) return 2;
            iv_hex = value;
        } else if (argument == "--output") {
            const char* value = require_value("--output");
            if (value == nullptr) return 2;
            output_path = text::to_wide(value);
        } else {
            std::fprintf(stderr, "unknown transform option: %s\n", argv[index]);
            return 2;
        }
    }

    if (algorithm.empty() || key_hex.empty() || output_path.empty()) {
        std::fprintf(stderr, "--algorithm, --key-hex and --output are required\n");
        return 2;
    }

    print_offline_banner("transform");
    std::string error;
    std::vector<BYTE> input;
    if (!read_file(input_path, input, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }

    const auto key = parse_hex(key_hex, error);
    if (!key) {
        std::fprintf(stderr, "[-] key: %s\n", error.c_str());
        return 2;
    }

    std::vector<BYTE> output;
    if (algorithm == "xor") {
        output.resize(input.size());
        for (size_t index = 0; index < input.size(); ++index) {
            output[index] = input[index] ^ (*key)[index % key->size()];
        }
    } else if (algorithm == "aes-256-cbc") {
        if (mode != "encrypt" && mode != "decrypt") {
            std::fprintf(stderr, "[-] AES requires --mode encrypt or decrypt\n");
            return 2;
        }
        if (iv_hex.empty()) {
            std::fprintf(stderr, "[-] AES requires --iv-hex\n");
            return 2;
        }
        const auto iv = parse_hex(iv_hex, error);
        if (!iv) {
            std::fprintf(stderr, "[-] IV: %s\n", error.c_str());
            return 2;
        }
        if (!aes_cbc_transform(input, *key, *iv, mode == "encrypt", output, error)) {
            std::fprintf(stderr, "[-] %s\n", error.c_str());
            return 1;
        }
    } else {
        std::fprintf(stderr, "[-] unsupported algorithm: %s\n", algorithm.c_str());
        return 2;
    }

    if (!write_file_atomic(output_path, output, error)) {
        std::fprintf(stderr, "[-] %s\n", error.c_str());
        return 1;
    }

    std::array<BYTE, 32> digest{};
    std::string hash;
    if (sha256(output, digest, error)) {
        hash = bytes_to_hex(digest.data(), digest.size());
    }
    std::printf("[+] transformed %zu -> %zu bytes\n", input.size(), output.size());
    std::printf("[+] saved %s\n", text::to_utf8(output_path.wstring()).c_str());
    if (!hash.empty()) {
        std::printf("[+] sha256 %s\n", hash.c_str());
    }
    return 0;
}
