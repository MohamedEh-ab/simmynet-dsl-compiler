#!/usr/bin/env python3
"""Verify the *contents* of built distributions before they are uploaded to PyPI.

    python scripts/check_wheel.py dist/simmynet-2.2.0*.whl dist/simmynet-2.2.0.tar.gz [--version 2.2.0]

Wheel checks   the hybrid package is complete: simmynet/__init__.py, simmynet/wrapper.py and exactly one
               compiled simmynet/_core.* extension; METADATA has the right name / version / license /
               Requires-Python; no bundled foreign libraries (a `*.libs/` directory or extra .so/.dll/
               .dylib files would mean auditwheel/delvewheel had to copy something in); no stray
               build products; nothing named after the old project.
sdist checks   everything CMake needs to rebuild from source is present (CMakeLists.txt, include/,
               src/, python/simmynet/*.py, LICENSE, CITATION.cff, scripts/, tests/).
"""
import argparse
import re
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OLD_NAME = "sym" + "net"   # spelled in two pieces so this file itself never contains the old name


def expected_version() -> str:
    m = re.search(r'^version\s*=\s*"([^"]+)"', (ROOT / "pyproject.toml").read_text(encoding="utf-8"), re.M)
    if not m:
        raise SystemExit("cannot read the version from pyproject.toml")
    return m.group(1)


def check_wheel(path: Path, version: str, allow_linux_tag: bool = False):
    errs = []
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        for need in ("simmynet/__init__.py", "simmynet/wrapper.py"):
            if need not in names:
                errs.append(f"missing {need}")
        ext = [n for n in names if re.fullmatch(r"simmynet/_core[^/]*\.(so|pyd)", n)]
        if len(ext) != 1:
            errs.append(f"expected exactly one simmynet/_core extension, found {ext}")
        dist_info = [n for n in names if n.endswith(".dist-info/METADATA")]
        if len(dist_info) != 1:
            errs.append("missing .dist-info/METADATA")
        else:
            meta = z.read(dist_info[0]).decode("utf-8")
            for key, val in (("Name", "simmynet"), ("Version", version), ("License-Expression", "MIT")):
                m = re.search(rf"^{key}:\s*(.+)$", meta, re.M)
                if not m or m.group(1).strip().lower() != val.lower():
                    errs.append(f"METADATA {key} is {m.group(1).strip() if m else None!r}, expected {val!r}")
            if not re.search(r"^Requires-Python:\s*>=3\.9", meta, re.M):
                errs.append("METADATA Requires-Python is not >=3.9")
        if not any(n.endswith(".dist-info/licenses/LICENSE") or n.endswith(".dist-info/LICENSE") for n in names):
            errs.append("LICENSE is not packaged in .dist-info")
        top = {n.split("/")[0] for n in names}
        stray = sorted(t for t in top if t not in ("simmynet",) and not t.endswith(".dist-info") and not t.endswith(".data"))
        if stray:
            errs.append(f"unexpected top-level entries (bundled libraries / build leftovers?): {stray}")
        foreign = [n for n in names if re.search(r"\.(dll|dylib|a|lib|o|obj)$", n) or
                   (n.endswith(".so") and n not in ext) or ".libs/" in n]
        if foreign:
            errs.append(f"foreign binaries inside the wheel: {foreign}")
        bad_names = [n for n in names if OLD_NAME in n.lower()]
        if bad_names:
            errs.append(f"entries still carry the old project name: {bad_names}")
        if not path.name.startswith(f"simmynet-{version}-"):
            errs.append(f"wheel file name {path.name!r} does not start with simmynet-{version}-")
        if not allow_linux_tag and re.search(r"-linux_\w+\.whl$", path.name):
            errs.append("plain 'linux_*' platform tag: PyPI rejects it, the wheel must be manylinux/musllinux")
    return errs


def check_sdist(path: Path, version: str):
    errs = []
    with tarfile.open(path) as t:
        names = [n.split("/", 1)[1] for n in t.getnames() if "/" in n]
    if not path.name.startswith(f"simmynet-{version}"):
        errs.append(f"sdist file name {path.name!r} does not start with simmynet-{version}")
    for need in ("CMakeLists.txt", "pyproject.toml", "setup.py", "LICENSE", "README.md", "CITATION.cff",
                 "include/simmynet/Version.hpp", "include/simmynet/finite_field/FrobeniusGenerator.hpp",
                 "src/bindings.cpp", "src/bind_finite_field.cpp", "src/bind_dsl.cpp",
                 "python/simmynet/__init__.py", "python/simmynet/wrapper.py",
                 "scripts/check_extension_deps.py", "tests/python/test_bindings.py", "tests/python/test_wrapper.py"):
        if need not in names:
            errs.append(f"sdist is missing {need}")
    if any(re.search(r"\.(so|pyd|dll|dylib|o|obj)$", n) for n in names):
        errs.append("sdist contains compiled binaries")
    bad = [n for n in names if OLD_NAME in n.lower()]
    if bad:
        errs.append(f"sdist entries carry the old project name: {bad}")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--version", default=None, help="expected version (default: read from pyproject.toml)")
    ap.add_argument("--allow-linux-tag", action="store_true", help="accept plain linux_* wheels (local, un-repaired builds)")
    args = ap.parse_args()
    version = args.version or expected_version()
    failed = False
    for f in args.files:
        p = Path(f)
        errs = check_wheel(p, version, args.allow_linux_tag) if p.suffix == ".whl" else check_sdist(p, version)
        print(("FAIL " if errs else "ok   ") + p.name)
        for e in errs:
            print("     - " + e)
        failed |= bool(errs)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
