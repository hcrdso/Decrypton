# Decrypton

**Windows x64 PE memory snapshotter, coverage reporter, minidump writer, and conservative import rebuilder.**

![Version](https://img.shields.io/badge/version-0.3.0-0ea5e9)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599c)
![License](https://img.shields.io/badge/license-AGPL--3.0-663399)
![Status](https://img.shields.io/badge/status-experimental-f59e0b)

</div>

> [!IMPORTANT]
> Use Decrypton only on software and processes you own or are explicitly authorized to inspect.

> [!WARNING]
> Version 0.3.0 is experimental. A structurally valid output file is not necessarily runnable. Always retain the original executable and inspect the JSON coverage report before relying on a dump.

## What changed in 0.3.0

- Removed the misleading `NtFlushInstructionCache` retry path.
- Never changes remote page protections and does not contain anti-tamper-specific bypass logic.
- Salvages partial `ReadProcessMemory` results by recursively subdividing a page down to 64-byte fragments.
- Keeps original on-disk bytes wherever memory cannot be read, producing an explicit hybrid image instead of silently zeroing data.
- Records every incomplete page in a JSON report with RVA, address, state, protection, requested bytes, and copied bytes.
- Refuses import rebuilding below a configurable code-coverage threshold unless explicitly forced.
- Rejects a rebuilt import table unless every candidate slot is reached by a supported RIP-relative reference, then restores the original import structures.
- Scans only import-pointer slots whose full eight bytes were actually copied from memory.
- Supports selecting any normally loaded module with `--module`.
- Supports listing loaded modules with `--list-modules`.
- Can create a standard Windows minidump through `MiniDumpWriteDump`.
- Uses read-only process access: `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`.

## Requirements

- Windows 10 or Windows 11, x64.
- Visual Studio 2022 with **Desktop development with C++**.
- Windows SDK.
- CMake 3.21 or newer.

Decrypton currently supports PE32+ / AMD64 images only.

## Building

From an **x64 Native Tools Command Prompt for Visual Studio 2022**:

```cmd
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will normally be written to:

```text
build\Release\decrypton.exe
```

Preset alternative:

```cmd
cmake --preset vs2022-x64
cmake --build --preset release
```

To rebuild from a clean directory:

```cmd
rmdir /S /Q build
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Usage

```text
decrypton.exe --process <name> [options]
decrypton.exe --pid <id> [options]
decrypton.exe [process.exe] [limit-percent]
```

### Options

| Option | Description |
|---|---|
| `-p, --process <name>` | Select the target process by executable name. |
| `--pid <id>` | Select the target process by PID. |
| `-m, --module <name>` | Dump a loaded module instead of the process main module. |
| `--list-modules` | List normally loaded modules and exit. |
| `-o, --output <path>` | Set the output PE path. |
| `-l, --limit <1-100>` | Copy only the selected percentage of each executable section. |
| `--min-coverage <1-100>` | Minimum copied-code coverage required before import rebuilding. Default: `80`. |
| `--no-data` | Keep non-executable sections from the disk image. |
| `--no-imports` | Preserve the original import structures. |
| `--aggressive-imports` | Broaden resolved-import scanning. May increase false positives. |
| `--force-imports` | Keep a rebuilt table even when normal safety checks reject it. |
| `--report <path>` | Set the JSON report path. |
| `--no-report` | Disable the JSON report. |
| `--minidump <path>` | Create a standard Windows minidump. |
| `--version` | Display the program version. |
| `-h, --help` | Display command-line help. |

## Examples

Dump the main module of a process:

```cmd
decrypton.exe --process Example.exe --output Example-dumped.exe
```

Dump a specific loaded DLL:

```cmd
decrypton.exe --process Example.exe --module ExampleCore.dll --output ExampleCore-dumped.dll
```

List modules first:

```cmd
decrypton.exe --pid 1234 --list-modules
```

Create a PE snapshot, JSON report, and Windows minidump:

```cmd
decrypton.exe --pid 1234 ^
  --output snapshot.exe ^
  --report snapshot.json ^
  --minidump snapshot.dmp
```

Preserve imports and copy only memory-backed executable data:

```cmd
decrypton.exe --process Example.exe --no-imports --no-data
```

Require at least 95% code coverage before import rebuilding:

```cmd
decrypton.exe --process Example.exe --min-coverage 95
```

## Reading the result

Decrypton starts with the original file on disk and overlays only bytes successfully read from the live process. Therefore:

- A page copied completely from memory represents the live image.
- A partially copied page contains a mixture of live bytes and original disk bytes.
- An unreadable page remains unchanged from the original disk image.
- The JSON report identifies every partial or failed page.

A result with low executable-section coverage should be treated as an analysis artifact, not as a reconstructed executable.

## Import reconstruction safeguards

Resolved imports are matched against named exports from loaded modules. Decrypton then attempts to rebuild a new import section and redirect supported x64 RIP-relative references.

By default, the rebuild is discarded when:

- copied code coverage is below `--min-coverage`;
- no reliable import slots are found;
- the PE has no room for another section header;
- the reconstructed section fails validation; or
- one or more candidate import slots have no supported RIP-relative code reference.

When a rebuild is discarded, Decrypton restores the original import descriptors, lookup thunks, IAT thunks, module names, and import-by-name records from the disk image.

`--force-imports` disables some of these conservative checks and should be used only for controlled research where manual validation is expected.

## Minidumps

`--minidump` uses the documented `MiniDumpWriteDump` API and includes:

- data and code segments;
- handle information;
- unloaded-module information;
- full memory-region metadata;
- thread information.

It is not a full-memory dump and may omit inaccessible memory.

## Known limitations

- PE32+ / AMD64 only.
- Does not enumerate manually mapped or hidden modules.
- Does not change page protections or force inaccessible pages to become readable.
- Does not implement anti-tamper-specific hooks, callbacks, drivers, or execution triggering.
- Export matching currently uses named, non-forwarded exports.
- RIP-relative repair is intentionally limited and is not a complete x86-64 decoder.
- Delay-import reconstruction is not implemented.
- Memory-only PE reconstruction and loose executable-region carving are not implemented.
- Self-modifying code can change while the snapshot is being captured.
- A generated PE may still require manual repair in a PE editor or reverse-engineering suite.

Decrypton 0.3.0 deliberately does **not** incorporate their anti-tamper-specific decryption behavior. The adopted design ideas are general-purpose concepts such as modular PE handling, coverage accounting, module selection, minidumps, conservative reconstruction, and detailed validation.

## Responsible use

Decrypton is intended for authorized reverse engineering, interoperability work, software preservation, incident response, malware analysis in isolated environments, and research on binaries you are permitted to inspect.

You are responsible for complying with applicable laws, licenses, platform policies, and contractual obligations.

## License

Decrypton is licensed under the **GNU Affero General Public License v3.0**.

## Author

Created by [hcrdso](https://github.com/hcrdso).
