"""Build script: bridges setuptools and CMake so `pip install .` compiles the pybind11 core.

All package metadata lives in pyproject.toml; this file only declares the C++ extension
`simmynet._core` and how to build it.

Environment variables
    SIMMYNET_NATIVE_ARCH=1   build with -march=native (fast, but the wheel only runs on similar CPUs;
                           OFF by default so wheels are portable; ignored, with a warning, when
                           CIBUILDWHEEL is set so a release wheel can never be built non-portably)
    SIMMYNET_STATIC_RUNTIME=0  link the extension against the shared C++ runtime instead of the static
                           one (default 1: no VC++ redistributable / libstdc++ dependency in the wheel)
    CMAKE_ARGS             extra arguments for the CMake configure step (shell-style quoting allowed)
    CMAKE_GENERATOR        generator override (Ninja is used automatically when the `ninja` package exists)
    CMAKE_BUILD_PARALLEL_LEVEL, DEBUG
"""
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

PLAT_TO_CMAKE = {"win32": "Win32", "win-amd64": "x64", "win-arm32": "ARM", "win-arm64": "ARM64"}


class CMakeExtension(Extension):
    """An extension whose sources are built by the project's CMakeLists.txt, not by setuptools."""

    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.fspath(Path(sourcedir).resolve())


def _truthy(value: str) -> bool:
    return value.strip().lower() not in ("", "0", "off", "false", "no")


def _native_arch_requested() -> bool:
    """SIMMYNET_NATIVE_ARCH=1 opts in to -march=native, except inside cibuildwheel (never for release wheels)."""
    wanted = _truthy(os.environ.get("SIMMYNET_NATIVE_ARCH", "0"))
    if wanted and os.environ.get("CIBUILDWHEEL"):
        print("warning: SIMMYNET_NATIVE_ARCH ignored under cibuildwheel (wheels must be portable)", file=sys.stderr)
        return False
    return wanted


class CMakeBuild(build_ext):
    def build_extension(self, ext: CMakeExtension) -> None:
        cmake = shutil.which("cmake")
        if cmake is None:
            raise RuntimeError(
                "CMake >= 3.18 was not found. `pip install .` normally provides it through the build "
                "requirements; with --no-build-isolation install it first (`pip install cmake`).")

        # <build_lib>/simmynet/_core.<ext>  ->  the directory that must receive the compiled module
        ext_fullpath = Path.cwd() / self.get_ext_fullpath(ext.name)
        extdir = ext_fullpath.parent.resolve()

        debug = int(os.environ.get("DEBUG", 0)) if self.debug is None else self.debug
        cfg = "Debug" if debug else "Release"
        cmake_generator = os.environ.get("CMAKE_GENERATOR", "")

        cmake_args = [
            f"-DSIMMYNET_PY_PACKAGE_DIR={extdir}",   # the CMake target writes _core.<ext> straight into the package
            "-DSIMMYNET_PY_COPY_SOURCES=OFF",        # setuptools packages the .py files itself
            "-DSIMMYNET_BUILD_PYTHON=ON",
            "-DSIMMYNET_REQUIRE_PYTHON=ON",          # fail loudly instead of silently skipping the module
            "-DSIMMYNET_BUILD_CLI=OFF",
            "-DSIMMYNET_BUILD_TESTS=OFF",
            "-DSIMMYNET_FETCH_PYBIND11=OFF",         # pybind11 comes from the (isolated) build environment
            f"-DSIMMYNET_NATIVE_ARCH={'ON' if _native_arch_requested() else 'OFF'}",   # portable unless opted in
            f"-DSIMMYNET_STATIC_RUNTIME={'ON' if _truthy(os.environ.get('SIMMYNET_STATIC_RUNTIME', '1')) else 'OFF'}",
            f"-DPython_EXECUTABLE={sys.executable}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
            f"-DCMAKE_BUILD_TYPE={cfg}",
        ]
        build_args = []
        if "CMAKE_ARGS" in os.environ:
            cmake_args += shlex.split(os.environ["CMAKE_ARGS"], posix=(os.name != "nt"))

        if self.compiler.compiler_type != "msvc":
            # prefer Ninja when available (the `ninja` build requirement provides it)
            if not cmake_generator or cmake_generator == "Ninja":
                try:
                    import ninja  # type: ignore

                    cmake_args += ["-GNinja", f"-DCMAKE_MAKE_PROGRAM:FILEPATH={Path(ninja.BIN_DIR) / 'ninja'}"]
                except ImportError:
                    pass
        else:
            single_config = any(x in cmake_generator for x in {"NMake", "Ninja"})
            contains_arch = any(x in cmake_generator for x in {"ARM", "Win64"})
            if not single_config and not contains_arch:
                if self.plat_name not in PLAT_TO_CMAKE:
                    raise RuntimeError(f"unsupported Windows platform {self.plat_name!r}; set CMAKE_GENERATOR/CMAKE_ARGS")
                cmake_args += ["-A", PLAT_TO_CMAKE[self.plat_name]]
            if not single_config:
                build_args += ["--config", cfg]

        if sys.platform.startswith("darwin"):
            archs = re.findall(r"-arch (\S+)", os.environ.get("ARCHFLAGS", ""))
            if archs:
                cmake_args += ["-DCMAKE_OSX_ARCHITECTURES={}".format(";".join(archs))]

        if "CMAKE_BUILD_PARALLEL_LEVEL" not in os.environ and getattr(self, "parallel", None):
            build_args += [f"-j{self.parallel}"]

        build_temp = Path(self.build_temp) / ext.name
        build_temp.mkdir(parents=True, exist_ok=True)

        subprocess.run([cmake, ext.sourcedir, *cmake_args], cwd=build_temp, check=True)
        subprocess.run([cmake, "--build", ".", "--target", "simmynet_core", *build_args], cwd=build_temp, check=True)


setup(
    ext_modules=[CMakeExtension("simmynet._core", sourcedir=".")],
    cmdclass={"build_ext": CMakeBuild},
    zip_safe=False,
)
