# Decrypton

**Windows x64 PE memory-image dumper and import rebuilder.**

[![Version](https://img.shields.io/badge/version-0.2.0-0ea5e9)](#)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4)](#requirements)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599c)](#building)
[![CMake](https://img.shields.io/badge/CMake-3.20%2B-064f8c)](#building)
[![Status](https://img.shields.io/badge/status-experimental-f59e0b)](#project-status)

Decrypton combines an executable's on-disk PE image with selected file-backed
pages from its live process image, then optionally discovers resolved imports,
reconstructs the import directory, and repairs common x64 RIP-relative IAT
references.

</div>

> [!WARNING]
> **Decrypton 0.2.0 is experimental and has not yet been validated across a
> representative set of real-world targets.** Keep the original executable,
> inspect every generated file, and do not assume that a successful dump is a
> fully runnable or analysis-ready PE.

## Features

* Targets a process by executable name or PID.
* Reads file-backed executable and data sections from the live process.
* Supports partial code-section dumping through a percentage limit.
* Handles partial `ReadProcessMemory` results page by page.
* Normalizes base relocations only for ranges copied from memory.
* Builds an export-address map from loaded process modules.
* Finds resolved x64 import slots using safe or aggressive scanning.
* Reconstructs separate ILT and IAT tables in a new `.dcrypt` section.
* Repairs supported x64 RIP-relative references to rebuilt IAT slots.
* Validates PE32+ headers, sections, offsets, alignments, and output structure.
* Clears stale checksum, bound-import, and Authenticode directory metadata.
* Uses RAII for Windows handles and reports detailed Win32 errors.
* Provides colored progress output and a structured command-line interface.

## Project status

Decrypton is currently a **proof-of-concept / research build**.

The project compiles as a Windows x64 C++20 application, but the current release
still needs runtime testing against different PE layouts, Windows versions,
compilers, protectors, and import patterns. Bug reports should include the full
console output, Windows version, build configuration, and a reproducible target
that you are authorized to inspect.

## Requirements

* Windows 10 or Windows 11, x64.
* Visual Studio 2022 with the **Desktop development with C++** workload.
* A recent Windows SDK.
* CMake 3.21 or newer.
* Permission to query and read the target process.

Administrator privileges may be required for some targets. Protected processes,
kernel components, and targets blocked by platform security are outside the
scope of this version.

## Building

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** and run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The Visual Studio generator normally places the executable at:

```text
build\Release\decrypton.exe
```

Optional installation:

```powershell
cmake --install build --config Release --prefix install
```

## Usage

```text
decrypton.exe [process.exe] [limit-percent]
decrypton.exe --process <name> [options]
decrypton.exe --pid <id> [options]
```

### Options

| Option                 | Description                                                      |
| ---------------------- | ---------------------------------------------------------------- |
| `-p, --process <name>` | Target executable name. Defaults to `RobloxPlayerBeta.exe`.      |
| `--pid <id>`           | Target a process directly by its PID.                            |
| `-o, --output <path>`  | Output PE path. Defaults to `<module>-dumped.exe`.               |
| `-l, --limit <1-100>`  | Percentage of each executable section to copy.                   |
| `--no-data`            | Do not refresh non-executable sections from memory.              |
| `--no-imports`         | Preserve the original import directory instead of rebuilding it. |
| `--aggressive-imports` | Also scan executable sections for resolved import slots.         |
| `-h, --help`           | Print command-line help.                                         |

The positional form remains available for compatibility:

```powershell
decrypton.exe Example.exe 100
```

This is equivalent to selecting `Example.exe` and copying 100% of each eligible
executable section.

## Examples

Dump a process by name using the default output filename:

```powershell
.\decrypton.exe --process Example.exe
```

Choose an explicit output path:

```powershell
.\decrypton.exe --process Example.exe --output .\dumps\Example-dumped.exe
```

Target a specific PID:

```powershell
.\decrypton.exe --pid 1234 --output .\dumps\process-1234.exe
```

Copy only the first 50% of each executable section:

```powershell
.\decrypton.exe --process Example.exe --limit 50
```

Skip non-executable sections and retain the original imports:

```powershell
.\decrypton.exe --pid 1234 --no-data --no-imports
```

Enable the broader import scan:

```powershell
.\decrypton.exe --process Example.exe --aggressive-imports
```

> [!NOTE]
> Aggressive import scanning can find slots outside conventional writable data
> sections, but it can also increase false positives. Start with the default
> safe mode and compare results before relying on aggressive output.

## How it works

1. Decrypton locates the target process and its main module.
2. It loads the original executable from disk as the base PE image.
3. Eligible file-backed section ranges are copied from the live process image.
4. Base relocations in copied ranges are normalized back to the preferred image
   base where possible.
5. When import rebuilding is enabled, exports from loaded modules are indexed by
   their resolved virtual addresses.
6. Candidate import slots are matched against that export map.
7. A new `.dcrypt` section is appended with import descriptors, ILT entries, IAT
   entries, module names, and `IMAGE_IMPORT_BY_NAME` records.
8. Supported x64 RIP-relative references are redirected to the new IAT slots.
9. The resulting PE is validated and written to disk.

## Output and verification

A generated file should be treated as an intermediate reverse-engineering
artifact. After dumping, inspect it with appropriate PE analysis tools and check:

* Section table and image alignment.
* Entry point and executable section contents.
* Import descriptors, ILT, IAT, and imported names.
* Base relocation behavior.
* RIP-relative calls, jumps, and data references.
* TLS, exception data, load configuration, and other directories relevant to
  the target.

Decrypton does not guarantee that Windows will load or execute every generated
image successfully.

## Known limitations

* PE32+ / x64 only; PE32 / x86 is rejected.
* Windows user-mode processes only.
* Import discovery currently relies on named exports.
* Forwarded exports and uncommon loader behavior may require additional work.
* RIP-relative repair covers common supported instruction forms, not a complete
  x86-64 decoder.
* Rebuilding imports requires free space for another section header.
* Memory-only regions without corresponding file-backed section space are not
  emitted as arbitrary new sections.
* Self-modifying code, delayed initialization, anti-analysis logic, and custom
  loaders can produce incomplete or inconsistent snapshots.
* A valid PE structure does not imply that the dumped program is runnable.

## Troubleshooting

### `OpenProcess failed`

Run the terminal with the necessary privileges and confirm that the target is a
normal user-mode process you are allowed to inspect. Security software or
Windows process protection may deny access.

### `main module lookup failed`

Confirm that the process is still running and that the selected PID or process
name refers to the expected x64 executable.

### `no imports found`

Try the default scan first, then compare it with `--aggressive-imports`. Some
targets may not expose resolved import slots in patterns recognized by this
release.

### `no room for another section header`

The original PE headers do not contain enough unused space for `.dcrypt`.
Decrypton currently refuses to move existing raw sections automatically.

### The output is valid but does not run

A memory dump may still depend on runtime state, TLS initialization, exception
metadata, loader fixups, dynamically resolved APIs, or unpacked memory that is
not represented by the original file layout.

## Roadmap

* Broader runtime testing and regression fixtures.
* Better forwarded-export handling.
* Ordinal import reconstruction.
* Delay-import analysis.
* More complete x86-64 instruction decoding for reference repair.
* Optional PE checksum generation.
* Structured report output for imports, sections, and failed pages.
* Automated CI builds for supported Windows toolchains.

## Contributing

Issues and pull requests are welcome. Keep changes focused, compile with the
warnings configured in `CMakeLists.txt`, and describe how the change was tested.

Useful bug-report details include:

* Decrypton version and commit.
* Windows edition and build number.
* Visual Studio, compiler, and CMake versions.
* Exact command used.
* Complete console output.
* Whether safe or aggressive import scanning was enabled.
* A minimal authorized reproduction case when possible.

## Responsible use

Decrypton is intended for legitimate reverse engineering, interoperability,
malware analysis in controlled environments, software preservation, and
research on binaries you own or are explicitly authorized to inspect.

Do not use this project to access, copy, modify, or redistribute software or
process data without authorization. You are responsible for complying with all
applicable laws, licenses, policies, and contractual obligations.

## Author

Created by [hcrdso](https://github.com/hcrdso).
