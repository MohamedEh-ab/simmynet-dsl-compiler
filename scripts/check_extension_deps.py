#!/usr/bin/env python3
"""Audit the compiled `simmynet._core` extension for external runtime-library dependencies.

    python scripts/check_extension_deps.py [path/to/_core.<ext>]

Without an argument the extension of the *installed* `simmynet` package is inspected.  Exit status 0
means the module depends only on system libraries; 1 means something that a clean user machine may not
have (VC++ redistributable DLLs, libstdc++/libgcc_s, a Homebrew dylib, ...) is imported.

  Linux   NEEDED entries read from the ELF .dynamic section (pure Python: no binutils required, so it
          also runs inside minimal manylinux / musllinux test containers).  Allowed: glibc / musl
          libc, libm, libpthread, libdl, librt, libutil and the dynamic loader.
  macOS   `otool -L`.  Allowed: /usr/lib/* (libSystem, libc++) and /System/*.
  Windows PE import table via `pefile` (pip install pefile).  Denied: the MSVC/UCRT runtime
          (vcruntime*, msvcp*, concrt*, vcomp*, ucrtbase, api-ms-win-crt-*), MinGW runtimes and anything
          that is not a system DLL or the Python DLL itself.  Skipped with a note if pefile is missing.
"""
import importlib.util
import os
import re
import struct
import subprocess
import sys

LINUX_ALLOWED = re.compile(
    r"^(lib(c|m|pthread|dl|rt|util)\.so(\.\d+)*|ld-linux[\w.-]*\.so(\.\d+)*|ld64\.so(\.\d+)*|"
    r"libc\.musl-[\w]+\.so(\.\d+)*|linux-vdso\.so(\.\d+)*)$")
WINDOWS_DENIED = re.compile(
    r"^(vcruntime|msvcp|msvcr|concrt|vcomp|vccorlib|ucrtbase|api-ms-win-crt-|libstdc\+\+|libgcc|libwinpthread|libgomp)",
    re.I)
WINDOWS_ALLOWED = re.compile(
    r"^(kernel32|user32|advapi32|shell32|ole32|oleaut32|ws2_32|bcrypt|ntdll|crypt32|python3\d*|api-ms-win-core-[\w-]+)\.dll$|"
    r"^python3\d*\.dll$", re.I)


def find_extension() -> str:
    spec = importlib.util.find_spec("simmynet._core")
    if spec is None or not spec.origin:
        raise SystemExit("cannot locate simmynet._core: install the wheel first or pass the extension path")
    return spec.origin


def elf_needed(path: str):
    """DT_NEEDED names of an ELF32/ELF64 little-endian shared object (pure Python)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF" or data[5] != 1:
        raise ValueError("not a little-endian ELF file")
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        if is64:
            _, sh_type, _, _, sh_off, sh_size, sh_link = struct.unpack_from("<IIQQQQI", data, off)
        else:
            _, sh_type, _, _, sh_off, sh_size, sh_link = struct.unpack_from("<IIIIIII", data, off)
        sections.append((sh_type, sh_off, sh_size, sh_link))
    needed = []
    for sh_type, sh_off, sh_size, sh_link in sections:
        if sh_type != 6:  # SHT_DYNAMIC
            continue
        str_off = sections[sh_link][1]
        step = 16 if is64 else 8
        fmt = "<qQ" if is64 else "<iI"
        for pos in range(sh_off, sh_off + sh_size, step):
            tag, val = struct.unpack_from(fmt, data, pos)
            if tag == 0:
                break
            if tag == 1:  # DT_NEEDED
                end = data.index(b"\0", str_off + val)
                needed.append(data[str_off + val:end].decode())
    return needed


def check_linux(path):
    needed = elf_needed(path)
    return needed, [n for n in needed if not LINUX_ALLOWED.match(n)]


def check_macos(path):
    out = subprocess.run(["otool", "-L", path], check=True, capture_output=True, text=True).stdout.splitlines()[1:]
    deps = [line.strip().split(" (")[0] for line in out if line.strip()]
    deps = [d for d in deps if os.path.basename(d) != os.path.basename(path)]   # the module's own install name
    return deps, [d for d in deps if not (d.startswith("/usr/lib/") or d.startswith("/System/"))]


def check_windows(path):
    try:
        import pefile  # type: ignore
    except ImportError:
        print("note: pefile is not installed; skipping the Windows import-table audit (pip install pefile)")
        return None, []
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    deps = [e.dll.decode() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])]
    bad = [d for d in deps if WINDOWS_DENIED.match(d) or not WINDOWS_ALLOWED.match(d)]
    return deps, bad


def main(argv) -> int:
    path = argv[1] if len(argv) > 1 else find_extension()
    path = os.path.abspath(path)
    if sys.platform.startswith("linux"):
        deps, bad = check_linux(path)
    elif sys.platform == "darwin":
        deps, bad = check_macos(path)
    elif sys.platform == "win32":
        deps, bad = check_windows(path)
    else:
        print(f"note: no dependency audit for platform {sys.platform!r}")
        return 0
    print(f"{os.path.basename(path)}: depends on {deps if deps is not None else 'n/a'}")
    if bad:
        print("ERROR: external runtime dependencies that a clean machine may not have: " + ", ".join(bad))
        return 1
    print("OK: no external runtime-library dependencies beyond the operating system")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
