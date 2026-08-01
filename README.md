# Decrypton

**Windows PE capture, reconstruction, verification, diffing and explicit-key offline transformation suite.**

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599c)
![CMake](https://img.shields.io/badge/CMake-3.21%2B-064f8c)
![Status](https://img.shields.io/badge/status-experimental-f59e0b)
![License](https://img.shields.io/badge/license-AGPL--3.0-blue)

</div>

> [!IMPORTANT]
> Decrypton is intended for binaries and processes you own or are explicitly authorized to analyze. It does not change remote page protections, bypass protected-process boundaries, disable security products, or include anti-tamper evasion.

## Scope

Decrypton operates in user mode using documented Windows APIs. The capture command reads only memory that Windows reports as accessible. Unreadable bytes remain sourced from the original disk image, and coverage is recorded in the JSON report.

The transform command is not automatic cryptanalysis. It requires the exact algorithm, key and IV supplied by the operator.

## Requirements

- Windows 10 or Windows 11 x64.
- Visual Studio 2022 with **Desktop development with C++**.
- Windows SDK.
- CMake 3.21 or newer.

## Build

From an x64 Developer Command Prompt, the simplest route is:

```cmd
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
```

Without presets:

```cmd
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is generated at:

```text
build\Release\decrypton.exe
```

## Commands

### Capture

```cmd
decrypton capture --process Example.exe --output Example-dumped.exe
```

Dump a selected DLL:

```cmd
decrypton capture --process Example.exe --module ExampleCore.dll --output ExampleCore-dumped.dll
```

Produce coverage JSON and a Windows minidump:

```cmd
decrypton capture --process Example.exe ^
  --output Example-dumped.exe ^
  --report Example-report.json ^
  --minidump Example.dmp
```

Conservative capture without import rebuilding:

```cmd
decrypton capture --process Example.exe --output Example-raw.exe --no-imports
```

The old 0.3 syntax remains valid:

```cmd
decrypton --process Example.exe --output Example-dumped.exe
```

Run `decrypton capture --help` for every capture option.

### Inspect

```cmd
decrypton inspect Example-dumped.exe
```

Save a machine-readable report:

```cmd
decrypton inspect Example-dumped.exe --json inspect.json
```

The output includes:

- SHA-256.
- PE32 or PE32+ format and machine.
- Image base and entry point.
- Section RVAs, raw ranges, characteristics and Shannon entropy.
- Populated PE data directories.

### Verify

```cmd
decrypton verify Example-dumped.exe
```

Strict mode treats warnings as failure:

```cmd
decrypton verify Example-dumped.exe --strict --json verify.json
```

The validator checks:

- DOS, NT and optional headers.
- File and section alignment.
- Raw and virtual section bounds and overlaps.
- `SizeOfHeaders` and `SizeOfImage`.
- Entry-point placement.
- Data-directory bounds.
- Import descriptors, thunk arrays and import names.
- Export tables.
- Base-relocation blocks.
- x64 exception-directory sizing.
- TLS-directory bounds.

Exit status is zero only when validation succeeds under the selected mode.

### Diff

```cmd
decrypton diff Original.exe Candidate.exe
```

With JSON and a custom block size:

```cmd
decrypton diff Original.exe Candidate.exe --block 4096 --json diff.json
```

The command reports:

- Both SHA-256 hashes and sizes.
- Total changed bytes.
- Contiguous changed ranges.
- Changed fixed-size blocks.
- Changed bytes for same-named PE sections.

A nonzero exit status means the files differ.

### Offline transform

Repeating-key XOR:

```cmd
decrypton transform encrypted.bin ^
  --algorithm xor ^
  --key-hex A55A10FF ^
  --output decoded.bin
```

AES-256-CBC decryption with PKCS#7 padding:

```cmd
decrypton transform encrypted.bin ^
  --algorithm aes-256-cbc ^
  --mode decrypt ^
  --key-hex 000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F ^
  --iv-hex 0F0E0D0C0B0A09080706050403020100 ^
  --output decoded.bin
```

AES encryption uses the same command with `--mode encrypt`.

> [!CAUTION]
> Command-line keys can be retained by shell history or process-observation tools. Use disposable laboratory environments and clear sensitive history when appropriate.

## Recommended workflow

```cmd
decrypton capture --process LabTarget.exe --output capture.exe --report capture.json
decrypton verify capture.exe --json verification.json
decrypton inspect capture.exe --json inspection.json
decrypton diff LabTarget-original.exe capture.exe --json differences.json
```

This separates acquisition from analysis and makes each decision auditable.

## Testing

CTest currently covers:

- Version and help dispatch.
- Structural verification of the built executable.
- AES-256-CBC encrypt/decrypt round-trip.
- XOR round-trip.

GitHub Actions builds and tests the project on `windows-2022`.

## Limitations

- Capture supports x64 PE32+ modules.
- Offline inspection and verification support PE32 and PE32+.
- Capture cannot read inaccessible, guarded or protected memory.
- Import reconstruction is heuristic and should always be verified afterward.
- AES-CBC transform expects PKCS#7-compatible padding.
- The verifier is intentionally conservative and may report unusual but loadable images.
- Decrypton does not recover unknown cryptographic keys or identify every custom transform automatically.

## Responsible use

Use Decrypton only for legitimate reverse engineering, incident response, interoperability, software preservation, controlled malware research, education, and authorized competitions. You are responsible for complying with applicable law, licenses, rules and scope limitations.

## Author

Created by **hcrdso**.
