#include <Windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ntdll.lib")

using NtFlushInstructionCache_t = NTSTATUS(NTAPI*)(HANDLE, PVOID, SIZE_T);

namespace con {
    constexpr const char* R = "\033[31m";
    constexpr const char* G = "\033[32m";
    constexpr const char* Y = "\033[33m";
    constexpr const char* C = "\033[36m";
    constexpr const char* B = "\033[1m";
    constexpr const char* X = "\033[0m";

    template<typename... Args>
    void info(const char* fmt, Args... args) {
        printf("%s%s[i]%s ", B, C, X);
        printf(fmt, args...);
    }
    void ok(const char* msg)   { printf("%s%s[+]%s %s\n", B, G, X, msg); }
    void fail(const char* msg) { printf("%s%s[-]%s %s\n", B, R, X, msg); }
    void warn(const char* msg) { printf("%s%s[!]%s %s\n", B, Y, X, msg); }
}

namespace proc {
    DWORD find(const wchar_t* name) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (!_wcsicmp(pe.szExeFile, name)) {
                    CloseHandle(snap);
                    return pe.th32ProcessID;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return 0;
    }

    bool get_module(HANDLE h, const wchar_t* name, PVOID* base, DWORD* size) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(h));
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(snap, &me)) {
            do {
                if (!_wcsicmp(me.szModule, name)) {
                    *base = me.modBaseAddr;
                    *size = me.modBaseSize;
                    CloseHandle(snap);
                    return true;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return false;
    }

    std::vector<MODULEENTRY32W> get_all_modules(HANDLE h) {
        std::vector<MODULEENTRY32W> result;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(h));
        if (snap == INVALID_HANDLE_VALUE) return result;
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(snap, &me)) {
            do result.push_back(me);
            while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return result;
    }

    std::wstring path(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return L"";
        WCHAR buf[MAX_PATH]{};
        DWORD len = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &len);
        CloseHandle(h);
        return ok ? std::wstring(buf) : L"";
    }
}

namespace mem {
    bool readable(HANDLE h, PVOID addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, addr, &mbi, sizeof(mbi))) return false;
        return mbi.State == MEM_COMMIT && mbi.Protect != PAGE_NOACCESS && mbi.Protect != 0;
    }

    SIZE_T read_page(HANDLE h, PVOID addr, SIZE_T max, BYTE* out) {
        SIZE_T read = 0;
        ReadProcessMemory(h, addr, out, max, &read);
        return read;
    }

    template<typename T>
    bool read(HANDLE h, PVOID addr, T* out) {
        SIZE_T r = 0;
        return ReadProcessMemory(h, addr, out, sizeof(T), &r) && r == sizeof(T);
    }
}

namespace pe {
    IMAGE_NT_HEADERS* get_nt_headers(std::vector<BYTE>& buf) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        return reinterpret_cast<IMAGE_NT_HEADERS*>(buf.data() + dos->e_lfanew);
    }

    DWORD offset_to_rva(std::vector<BYTE>& buf, DWORD offset) {
        auto* nt = get_nt_headers(buf);
        if (!nt) return 0;
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (offset >= sec[i].PointerToRawData &&
                offset < sec[i].PointerToRawData + sec[i].SizeOfRawData) {
                return sec[i].VirtualAddress + (offset - sec[i].PointerToRawData);
            }
        }
        return 0;
    }
}

namespace imports {

struct FoundImport {
    uint64_t address;
    uint32_t rva;
    std::string module;
    std::string name;
};

std::unordered_map<uint64_t, std::pair<std::string, std::string>>build_export_map(HANDLE h) {
    std::unordered_map<uint64_t, std::pair<std::string, std::string>> map;
    auto modules = proc::get_all_modules(h);

    for (const auto& mod : modules) {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS nt{};
        if (!mem::read(h, mod.modBaseAddr, &dos)) continue;
        if (!mem::read(h, reinterpret_cast<BYTE*>(mod.modBaseAddr) + dos.e_lfanew, &nt)) continue;

        auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.Size == 0) continue;

        std::vector<BYTE> data(dir.Size);
        if (!ReadProcessMemory(h, reinterpret_cast<BYTE*>(mod.modBaseAddr) + dir.VirtualAddress, data.data(), dir.Size, nullptr))
            continue;

        auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(data.data());
        if (!exp->NumberOfFunctions) continue;

        auto* names = reinterpret_cast<DWORD*>(data.data() + exp->AddressOfNames - dir.VirtualAddress);
        auto* funcs = reinterpret_cast<DWORD*>(data.data() + exp->AddressOfFunctions - dir.VirtualAddress);
        auto* ords  = reinterpret_cast<WORD*>(data.data() + exp->AddressOfNameOrdinals - dir.VirtualAddress);

        char modname[256]{};
        WideCharToMultiByte(CP_UTF8, 0, mod.szModule, -1, modname, sizeof(modname), nullptr, nullptr);

        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (ords[i] >= exp->NumberOfFunctions) continue;
            const char* name = reinterpret_cast<const char*>(data.data() + names[i] - dir.VirtualAddress);
            uint64_t addr = reinterpret_cast<uint64_t>(mod.modBaseAddr) + funcs[ords[i]];
            map[addr] = { modname, name };
        }
    }
    return map;
}

std::vector<FoundImport> find_imports_aggressive(
    std::vector<BYTE>& buf,
    const std::unordered_map<uint64_t, std::pair<std::string, std::string>>& exports)
{
    std::vector<FoundImport> result;
    std::unordered_set<uint32_t> seen_rva;

    auto* nt = pe::get_nt_headers(buf);
    if (!nt) return result;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!sec[i].PointerToRawData || !sec[i].SizeOfRawData) continue;

        DWORD start = sec[i].PointerToRawData;
        DWORD end   = (std::min)(start + sec[i].SizeOfRawData, (DWORD)buf.size());

        for (DWORD pos = start; pos + 8 <= end; pos += 8) {
            uint64_t val = *reinterpret_cast<uint64_t*>(buf.data() + pos);
            if (val < 0x10000) continue;

            auto it = exports.find(val);
            if (it == exports.end()) continue;

            uint32_t rva = pe::offset_to_rva(buf, pos);
            if (!rva || seen_rva.count(rva)) continue;

            result.push_back({ val, rva, it->second.first, it->second.second });
            seen_rva.insert(rva);
        }
    }

    std::sort(result.begin(), result.end(),
        [](const FoundImport& a, const FoundImport& b) { return a.rva < b.rva; });

    return result;
}

void fix_rip_relative_references(std::vector<BYTE>& buf, const std::unordered_map<uint32_t, uint32_t>& old_iat_to_new_thunk)
{
    if (old_iat_to_new_thunk.empty()) return;

    auto* nt = pe::get_nt_headers(buf);
    if (!nt) return;

    size_t patched = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (!sec[i].PointerToRawData || !sec[i].SizeOfRawData) continue;

        DWORD start = sec[i].PointerToRawData;
        DWORD end   = (std::min)(start + sec[i].SizeOfRawData, (DWORD)buf.size());

        for (DWORD pos = start; pos + 6 <= end; ++pos) {
            if (buf[pos] != 0xFF) continue;
            if (buf[pos + 1] != 0x15 && buf[pos + 1] != 0x25) continue;

            int32_t disp = *reinterpret_cast<int32_t*>(&buf[pos + 2]);
            uint32_t instr_rva = pe::offset_to_rva(buf, pos);
            if (!instr_rva) continue;

            uint32_t target_rva = instr_rva + 6 + disp;

            auto it = old_iat_to_new_thunk.find(target_rva);
            if (it == old_iat_to_new_thunk.end()) continue;

            int32_t new_disp = static_cast<int32_t>(it->second - (instr_rva + 6));
            *reinterpret_cast<int32_t*>(&buf[pos + 2]) = new_disp;
            ++patched;
        }
    }

    if (patched)
        con::ok(("patched " + std::to_string(patched) + " rip-relative IAT references").c_str());
    else
        con::warn("no rip-relative IAT references found to patch");
}

bool rebuild_imports(std::vector<BYTE>& buf, std::vector<FoundImport>& found) {
    if (found.empty()) {
        con::warn("no imports to rebuild");
        return false;
    }

    con::info("rebuilding %zu imports...\n", found.size());

    std::map<std::string, std::vector<FoundImport*>> by_module;
    for (auto& imp : found)
        by_module[imp.module].push_back(&imp);

    auto* nt = pe::get_nt_headers(buf);
    if (!nt) return false;

    DWORD file_align = nt->OptionalHeader.FileAlignment ? nt->OptionalHeader.FileAlignment : 0x200;
    DWORD sect_align = nt->OptionalHeader.SectionAlignment ? nt->OptionalHeader.SectionAlignment : 0x1000;

    DWORD desc_count = static_cast<DWORD>(by_module.size()) + 1;
    DWORD desc_size  = desc_count * sizeof(IMAGE_IMPORT_DESCRIPTOR);

    DWORD thunk_size = 0;
    DWORD name_size  = 0;
    for (auto& [mod, entries] : by_module) {
        thunk_size += static_cast<DWORD>((entries.size() + 1) * 8);
        name_size  += static_cast<DWORD>(mod.size() + 1);
        for (auto* e : entries)
            name_size += 2 + static_cast<DWORD>(e->name.size() + 1);
    }

    DWORD total_size = (desc_size + thunk_size + name_size + file_align - 1) / file_align * file_align;
    DWORD new_raw = static_cast<DWORD>((buf.size() + file_align - 1) / file_align * file_align);

    auto* sec  = IMAGE_FIRST_SECTION(nt);
    auto* last = &sec[nt->FileHeader.NumberOfSections - 1];
    DWORD new_rva = (last->VirtualAddress + last->Misc.VirtualSize + sect_align - 1) / sect_align * sect_align;

    con::info("new section raw=0x%X rva=0x%X size=%u\n", new_raw, new_rva, total_size);

    size_t old_size = buf.size();
    if (new_raw + total_size > buf.size())
        buf.resize(new_raw + total_size, 0);

    nt  = pe::get_nt_headers(buf);
    sec = IMAGE_FIRST_SECTION(nt);

    auto* new_sec = &sec[nt->FileHeader.NumberOfSections];
    memset(new_sec, 0, sizeof(*new_sec));
    memcpy(new_sec->Name, ".hetalia", 8);
    new_sec->VirtualAddress   = new_rva;
    new_sec->Misc.VirtualSize = (total_size + sect_align - 1) / sect_align * sect_align;
    new_sec->PointerToRawData = new_raw;
    new_sec->SizeOfRawData    = total_size;
    new_sec->Characteristics  = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    nt->FileHeader.NumberOfSections++;
    nt->OptionalHeader.SizeOfImage = new_sec->VirtualAddress + new_sec->Misc.VirtualSize;

    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress = 0;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size = 0;

    con::info("buffer: %zu → %zu\n", old_size, buf.size());

    BYTE* data = buf.data() + new_raw;
    DWORD thunk_pos = desc_size;
    DWORD name_pos  = desc_size + thunk_size;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(data);
    int di = 0;

    std::unordered_map<uint32_t, uint32_t> old_iat_to_new_thunk;

    for (auto& [mod, entries] : by_module) {
        printf("%-22s %zu\n", mod.c_str(), entries.size());

        memcpy(data + name_pos, mod.c_str(), mod.size() + 1);
        desc[di].Name = new_rva + name_pos;
        name_pos += static_cast<DWORD>(mod.size() + 1);

        desc[di].OriginalFirstThunk = new_rva + thunk_pos;
        desc[di].FirstThunk         = new_rva + thunk_pos;

        auto* thunks = reinterpret_cast<uint64_t*>(data + thunk_pos);

        for (size_t i = 0; i < entries.size(); ++i) {
            WORD hint = 0;
            memcpy(data + name_pos, &hint, 2);
            memcpy(data + name_pos + 2, entries[i]->name.c_str(), entries[i]->name.size() + 1);

            uint32_t name_rva = new_rva + name_pos;
            thunks[i] = name_rva;
            old_iat_to_new_thunk[entries[i]->rva] = name_rva;

            name_pos += 2 + static_cast<DWORD>(entries[i]->name.size() + 1);
        }

        thunks[entries.size()] = 0;
        thunk_pos += static_cast<DWORD>((entries.size() + 1) * 8);
        ++di;
    }

    memset(&desc[di], 0, sizeof(IMAGE_IMPORT_DESCRIPTOR));

    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = new_rva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = desc_size;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = new_rva + desc_size;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = thunk_size;

    fix_rip_relative_references(buf, old_iat_to_new_thunk);

    con::ok("import directory rebuilt + references fixed");
    return true;
}

} // namespace imports

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring result(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &result[0], size);
    return result;
}

int main(int argc, char* argv[]) {
    std::string process_name = "RobloxPlayerBeta.exe";
    float limit = 1.0f;

    if (argc >= 2) {
        process_name = argv[1];
    }
    if (argc >= 3) {
        limit = static_cast<float>(atof(argv[2])) / 100.0f;
        if (limit <= 0.0f || limit > 1.0f) limit = 1.0f;
    }

    std::string output_name = process_name;
    if (output_name.size() > 4 && output_name.substr(output_name.size() - 4) == ".exe") {
        output_name = output_name.substr(0, output_name.size() - 4);
    }
    output_name += "-dumped.bin";

    printf("Made by hcrdso (github.com/hcrdso)\n");
    con::info("target process: %s\n", process_name.c_str());
    con::info("decryption limit: %.0f%%\n", limit * 100.0f);
    con::info("output file: %s\n", output_name.c_str());

    std::wstring wprocess = to_wide(process_name);

    DWORD pid = proc::find(wprocess.c_str());
    if (!pid) {
        con::fail("process not running");
        return 1;
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
    if (!h) {
        con::fail("OpenProcess failed");
        return 1;
    }

    auto NtFlush = reinterpret_cast<NtFlushInstructionCache_t>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtFlushInstructionCache"));

    PVOID base = nullptr;
    DWORD module_size = 0;
    if (!proc::get_module(h, wprocess.c_str(), &base, &module_size)) {
        con::fail("main module not found");
        CloseHandle(h);
        return 1;
    }

    auto disk_path = proc::path(pid);
    con::info("%s @ 0x%p (%u bytes)\n", process_name.c_str(), base, module_size);
    wprintf(L"disk: %s\n", disk_path.c_str());

    std::vector<BYTE> buf;
    {
        HANDLE fh = CreateFileW(disk_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) {
            con::fail("cannot open disk file");
            CloseHandle(h);
            return 1;
        }
        DWORD fs = GetFileSize(fh, nullptr);
        buf.resize(fs);
        DWORD rd = 0;
        ReadFile(fh, buf.data(), fs, &rd, nullptr);
        CloseHandle(fh);
        buf.resize(rd);
    }

    con::ok(("loaded " + std::to_string(buf.size()) + " bytes from disk").c_str());

    auto* nt0 = pe::get_nt_headers(buf);
    if (!nt0) {
        con::fail("invalid PE");
        CloseHandle(h);
        return 1;
    }

    auto* sec0 = IMAGE_FIRST_SECTION(nt0);
    SIZE_T total_patched = 0, total_code = 0;

    for (WORD i = 0; i < nt0->FileHeader.NumberOfSections; ++i) {
        if (!(sec0[i].Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        if (!sec0[i].PointerToRawData) continue;

        DWORD rva = sec0[i].VirtualAddress;
        DWORD raw = sec0[i].PointerToRawData;
        DWORD vsize = sec0[i].Misc.VirtualSize;
        SIZE_T max_readable = (std::min)((SIZE_T)vsize, buf.size() - raw);

        printf("code rva=0x%X size=%u\n", rva, vsize);

        SIZE_T already = 0, flushed = 0;
        SIZE_T pages = (max_readable + 0xFFF) / 0x1000;
        SIZE_T target = (SIZE_T)(pages * limit);

        for (SIZE_T off = 0; off < max_readable; off += 0x1000) {
            if (off / 0x1000 >= target) break;

            auto addr = (BYTE*)base + rva + off;
            SIZE_T chunk = (std::min)((SIZE_T)0x1000, max_readable - off);

            if (mem::readable(h, addr)) {
                total_patched += mem::read_page(h, addr, chunk, buf.data() + raw + off);
                ++already;
            } else if (NtFlush) {
                NtFlush(h, addr, chunk);
                ++flushed;
                if (mem::readable(h, addr))
                    total_patched += mem::read_page(h, addr, chunk, buf.data() + raw + off);
            }

            printf("\r[%5zu/%5zu] %3.0f%% read=%zu flush=%zu", off/0x1000+1, pages, 100.0*(off+chunk)/max_readable, already, flushed);
            fflush(stdout);
        }
        printf("\n");
        total_code += max_readable;
    }

    con::ok(("patched " + std::to_string(total_patched) + " / " + std::to_string(total_code) + " bytes").c_str());

    con::info("flushing data sections...\n");
    for (WORD i = 0; i < nt0->FileHeader.NumberOfSections; ++i) {
        if (sec0[i].Characteristics & IMAGE_SCN_CNT_CODE) continue;
        if (!sec0[i].PointerToRawData || !sec0[i].SizeOfRawData) continue;

        DWORD raw  = sec0[i].PointerToRawData;
        DWORD size = (std::min)(sec0[i].SizeOfRawData, sec0[i].Misc.VirtualSize);
        DWORD rva  = sec0[i].VirtualAddress;

        char name[9]{};
        memcpy(name, sec0[i].Name, 8);
        printf("%-10s ", name);

        for (SIZE_T off = 0; off < size; off += 0x1000) {
            auto addr = (BYTE*)base + rva + off;
            SIZE_T chunk = (std::min)((SIZE_T)0x1000, size - off);
            if (!mem::readable(h, addr) && NtFlush) NtFlush(h, addr, chunk);
            if (mem::readable(h, addr))
                mem::read_page(h, addr, chunk, buf.data() + raw + off);
        }
        printf("done\n");
    }

    con::info("building export map...\n");
    auto exports = imports::build_export_map(h);
    printf("%zu exports\n", exports.size());

    con::info("scanning imports (aggressive)...\n");
    auto found = imports::find_imports_aggressive(buf, exports);
    printf("%zu imports found\n", found.size());

    if (!found.empty())
        imports::rebuild_imports(buf, found);

    con::info("saving...\n");
    std::ofstream out(output_name, std::ios::binary);
    if (!out) {
        con::fail("cannot create output file");
        CloseHandle(h);
        return 1;
    }
    out.write((char*)buf.data(), buf.size());
    out.close();

    con::ok(("saved " + output_name + " (" + std::to_string(buf.size()) + " bytes)").c_str());
    CloseHandle(h);
    return 0;
}
