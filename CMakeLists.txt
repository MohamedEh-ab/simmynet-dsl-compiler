cmake_minimum_required(VERSION 3.18)
project(simmynet
        VERSION 2.2.0
        DESCRIPTION "Constant-folding, SIMD-friendly C++17 engine for the SimmyNet DSL"
        LANGUAGES CXX)

# ----------------------------------------------------------------------------------------
# Version guard: include/simmynet/Version.hpp is the C++ source of truth and must match
# project(VERSION).  (pyproject.toml / CITATION.cff are checked by tests/python/test_bindings.py.)
# ----------------------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/simmynet/Version.hpp" _simmynet_version_hpp)
string(REGEX MATCH "#define SIMMYNET_VERSION_STRING \"([0-9.]+)\"" _simmynet_version_match "${_simmynet_version_hpp}")
if(NOT _simmynet_version_match OR NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR "include/simmynet/Version.hpp says '${CMAKE_MATCH_1}' but project(VERSION) is "
                      "'${PROJECT_VERSION}': keep them in sync")
endif()

# ----------------------------------------------------------------------------------------
# Options
#
# SIMMYNET_NATIVE_ARCH (-march=native / /arch:AVX2) makes binaries that may crash with
# "Illegal instruction" on older CPUs, so it must never reach a distributable wheel:
#   * local/source CMake builds        -> ON  (fast, tuned for this machine)
#   * pip / setup.py / cibuildwheel    -> OFF (setup.py passes OFF explicitly; the CIBUILDWHEEL and
#                                              SIMMYNET_WHEEL_BUILD environment variables also flip
#                                              the default to OFF)
# Opt in to a native pip build with  SIMMYNET_NATIVE_ARCH=1 pip install .
# ----------------------------------------------------------------------------------------
if(DEFINED ENV{CIBUILDWHEEL} OR DEFINED ENV{SIMMYNET_WHEEL_BUILD})
  set(_simmynet_native_default OFF)
else()
  set(_simmynet_native_default ON)
endif()
option(SIMMYNET_NATIVE_ARCH    "Compile in-tree targets with -march=native (non-portable binaries)"  ${_simmynet_native_default})
option(SIMMYNET_STRICT_FP      "Add -ffp-contract=off for bit-reproducible floating point"           OFF)
option(SIMMYNET_BUILD_CLI      "Build the simmynet_cli executable"                                     ON)
option(SIMMYNET_BUILD_PYTHON   "Build the Python extension module (needs pybind11)"                  ON)
option(SIMMYNET_FETCH_PYBIND11 "Download pybind11 with FetchContent if it is not found"              ON)
option(SIMMYNET_BUILD_TESTS    "Build and register the tests"                                        ON)
option(SIMMYNET_REQUIRE_PYTHON "Fail the configure step if the Python extension cannot be built (used by pip)" OFF)
option(SIMMYNET_PY_COPY_SOURCES "Copy python/simmynet/*.py next to the built extension"         ON)
option(SIMMYNET_BUILD_LEAN_FFI "Build C ABI shared library for Lean 4 FFI"                                OFF)
option(SIMMYNET_STATIC_RUNTIME "Link the Python extension against a static C++ runtime (MSVC /MT, libstdc++/libgcc on Linux) so wheels need no VC++ redistributable or newer libstdc++" ON)
set(SIMMYNET_PY_PACKAGE_DIR "${CMAKE_BINARY_DIR}/python/simmynet" CACHE PATH
    "Directory that receives the compiled extension _core (setup.py points this at its build dir)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# ----------------------------------------------------------------------------------------
# Internal build flags. They reach in-tree targets only ($<BUILD_INTERFACE>), so an installed
# SimmyNet::native_cpp never forces -march=native onto downstream projects.
#
# Native tuning is refused (with a warning) wherever it cannot be honoured or would not be
# portable by construction: cross-compiles, macOS builds that name target architectures
# (universal2 / explicit arm64 or x86_64), Apple Silicon (no -march=native), and MSVC targets
# other than x86/x64 (/arch:AVX2 does not exist on ARM64).
# ----------------------------------------------------------------------------------------
set(_simmynet_use_native ${SIMMYNET_NATIVE_ARCH})
if(_simmynet_use_native)
  if(CMAKE_CROSSCOMPILING)
    message(WARNING "SIMMYNET_NATIVE_ARCH ignored: cross-compiling")
    set(_simmynet_use_native OFF)
  elseif(APPLE AND (CMAKE_OSX_ARCHITECTURES OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
    message(STATUS "SIMMYNET_NATIVE_ARCH ignored on Apple targets that name an architecture / arm64")
    set(_simmynet_use_native OFF)
  elseif(MSVC AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|X64|x86|X86|AMD64)$")
    message(STATUS "SIMMYNET_NATIVE_ARCH ignored: /arch:AVX2 needs an x86/x64 MSVC target "
                   "(got '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}')")
    set(_simmynet_use_native OFF)
  endif()
endif()

add_library(simmynet_native_flags INTERFACE)
if(MSVC)
  # /utf-8: sources contain non-ASCII characters in comments; without it MSVC decodes them with the
  # ANSI code page (C4819 warnings, and on East-Asian code pages a real parsing hazard).
  target_compile_options(simmynet_native_flags INTERFACE /W4 /O2 /utf-8
      $<$<BOOL:${_simmynet_use_native}>:/arch:AVX2>
      $<$<BOOL:${SIMMYNET_STRICT_FP}>:/fp:strict>)
else()
  target_compile_options(simmynet_native_flags INTERFACE
      -Wall -Wextra -Wpedantic
      $<$<NOT:$<CONFIG:Debug>>:-O3>
      $<$<BOOL:${_simmynet_use_native}>:-march=native>
      $<$<BOOL:${SIMMYNET_STRICT_FP}>:-ffp-contract=off>)
endif()

# ----------------------------------------------------------------------------------------
# Header-only library:  target_link_libraries(app PRIVATE SimmyNet::native_cpp)
# ----------------------------------------------------------------------------------------
add_library(simmynet_native_cpp INTERFACE)
add_library(SimmyNet::native_cpp ALIAS simmynet_native_cpp)
set_target_properties(simmynet_native_cpp PROPERTIES EXPORT_NAME native_cpp)
target_include_directories(simmynet_native_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(simmynet_native_cpp INTERFACE cxx_std_17)
target_link_libraries(simmynet_native_cpp INTERFACE $<BUILD_INTERFACE:simmynet_native_flags>)

# ----------------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------------
if(SIMMYNET_BUILD_CLI)
  add_executable(simmynet_cli src/main.cpp)
  target_link_libraries(simmynet_cli PRIVATE SimmyNet::native_cpp)
  install(TARGETS simmynet_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ----------------------------------------------------------------------------------------
# Python extension module `simmynet`
# ----------------------------------------------------------------------------------------
if(SIMMYNET_BUILD_PYTHON)
  find_package(Python 3.9 COMPONENTS Interpreter Development.Module QUIET)
  if(Python_FOUND)
    set(PYBIND11_FINDPYTHON ON)  # make pybind11 reuse the interpreter found above

    # `pip install pybind11` is invisible to CMake; ask the interpreter where its config lives.
    if(NOT pybind11_DIR)
      execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                      OUTPUT_VARIABLE _simmynet_pb11_dir OUTPUT_STRIP_TRAILING_WHITESPACE
                      RESULT_VARIABLE _simmynet_pb11_rc ERROR_QUIET)
      if(_simmynet_pb11_rc EQUAL 0 AND EXISTS "${_simmynet_pb11_dir}")
        set(pybind11_DIR "${_simmynet_pb11_dir}" CACHE PATH "pybind11 CMake config directory")
      endif()
    endif()

    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND AND SIMMYNET_FETCH_PYBIND11)
      message(STATUS "pybind11 not found locally - fetching v2.13.6 with FetchContent")
      include(FetchContent)
      FetchContent_Declare(pybind11
          GIT_REPOSITORY https://github.com/pybind/pybind11.git
          GIT_TAG        v2.13.6
          GIT_SHALLOW    TRUE)
      FetchContent_MakeAvailable(pybind11)
      set(pybind11_FOUND TRUE)
    endif()

    if(pybind11_FOUND)
      # The compiled core is simmynet/_core.<ext>; python/simmynet/__init__.py re-exports it
      # and adds the pure-Python wrapper layer.
      pybind11_add_module(simmynet_core MODULE
          src/bindings.cpp src/bind_dsl.cpp src/bind_finite_field.cpp)
      target_link_libraries(simmynet_core PRIVATE SimmyNet::native_cpp)
      set_target_properties(simmynet_core PROPERTIES
          OUTPUT_NAME "_core"
          LIBRARY_OUTPUT_DIRECTORY "$<1:${SIMMYNET_PY_PACKAGE_DIR}>")  # $<1:..> stops MSVC adding Release/

      # Zero external runtime DLLs/SOs in the wheel (verified by scripts/check_extension_deps.py):
      #   MSVC     : /MT statically links the CRT + C++ standard library, so the module does not import
      #              VCRUNTIME140*.dll / MSVCP140*.dll (a clean machine without the VC++ redistributable
      #              would otherwise fail with "DLL load failed"). Nothing from the CRT crosses the module
      #              boundary: pybind11 copies values into Python objects and exceptions are translated here.
      #   Linux/MinGW: libstdc++ and libgcc are linked statically (no libstdc++.so.6 / libgcc_s.so.1
      #              version skew); libc/libm stay dynamic, as the manylinux policy requires.
      #   macOS    : libc++/libSystem are part of the OS, nothing to do.
      if(SIMMYNET_STATIC_RUNTIME)
        if(MSVC)
          set_property(TARGET simmynet_core PROPERTY
              MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        elseif(NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
          target_link_options(simmynet_core PRIVATE -static-libstdc++ -static-libgcc)
        endif()
      endif()
      if(SIMMYNET_PY_COPY_SOURCES)
        add_custom_command(TARGET simmynet_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/python/simmynet" "${SIMMYNET_PY_PACKAGE_DIR}"
            COMMENT "Copying the simmynet Python package next to the extension")
      endif()
      set(SIMMYNET_PYTHON_INSTALL_DIR
          "${CMAKE_INSTALL_LIBDIR}/python${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}/site-packages"
          CACHE STRING "Install destination of the Python package (relative to the install prefix)")
      install(TARGETS simmynet_core LIBRARY DESTINATION ${SIMMYNET_PYTHON_INSTALL_DIR}/simmynet)
      install(DIRECTORY python/simmynet DESTINATION ${SIMMYNET_PYTHON_INSTALL_DIR}
              FILES_MATCHING PATTERN "*.py")
    else()
      set(_simmynet_sev WARNING)
      if(SIMMYNET_REQUIRE_PYTHON)
        set(_simmynet_sev FATAL_ERROR)
      endif()
      message(${_simmynet_sev} "pybind11 not found and SIMMYNET_FETCH_PYBIND11=OFF: skipping the Python module. "
                      "Run `pip install pybind11` or pass -Dpybind11_DIR=<dir>.")
    endif()
  else()
    set(_simmynet_sev WARNING)
    if(SIMMYNET_REQUIRE_PYTHON)
      set(_simmynet_sev FATAL_ERROR)
    endif()
    message(${_simmynet_sev} "Python 3 interpreter/development files not found: skipping the Python module.")
  endif()
endif()

# ----------------------------------------------------------------------------------------
# Lean 4 bridge (opt-in; OFF by default and independent of every target above)
#
# simmynet_lean is a SHARED library of `extern "C"` functions (uint64_t / int64_t / double only)
# that bridges/lean4/SimmyFFI.lean declares with @[extern]; see bridges/lean4/README.md.  It does not
# touch simmynet_core (the Python extension) or its static-runtime settings.
# ----------------------------------------------------------------------------------------
if(SIMMYNET_BUILD_LEAN_FFI)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bridges/lean4/simmy_lean_ffi.cpp")
    message(FATAL_ERROR "SIMMYNET_BUILD_LEAN_FFI=ON needs bridges/lean4/ (it is not part of the sdist); use a git checkout")
  endif()
  add_library(simmynet_lean SHARED bridges/lean4/simmy_lean_ffi.cpp)
  target_link_libraries(simmynet_lean PRIVATE SimmyNet::native_cpp)
  set_target_properties(simmynet_lean PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      # lakefile.toml links -L../../build/bridges/lean4 (relative to bridges/lean4)
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bridges/lean4"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bridges/lean4")
  # Lean's toolchain brings its own C++ runtime; keep ours private so the two never meet.
  if(SIMMYNET_STATIC_RUNTIME AND NOT APPLE AND NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(simmynet_lean PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  install(TARGETS simmynet_lean LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  if(SIMMYNET_BUILD_TESTS)
    enable_testing()
    # The C ABI against the simmynet::ff functions it wraps.
    add_executable(lean_ffi_abi_test bridges/lean4/tests/ffi_abi_test.cpp)
    target_link_libraries(lean_ffi_abi_test PRIVATE simmynet_lean SimmyNet::native_cpp)
    add_test(NAME simmynet_lean_ffi_abi COMMAND lean_ffi_abi_test)

    # The generated, header-only LeanVerifiedRules.hpp: hooks, negative controls, static_asserts.
    add_executable(lean_verified_rules_test bridges/lean4/tests/verified_rules_test.cpp)
    target_link_libraries(lean_verified_rules_test PRIVATE SimmyNet::native_cpp)
    add_test(NAME simmynet_lean_verified_rules COMMAND lean_verified_rules_test)

    # The committed header must be exactly what lean_to_cpp.py generates from the committed rules.json.
    find_program(SIMMYNET_PYTHON_EXE NAMES python3 python)
    if(SIMMYNET_PYTHON_EXE)
      add_test(NAME simmynet_lean_header_fresh
               COMMAND "${SIMMYNET_PYTHON_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/bridges/lean4/lean_to_cpp.py"
                       "${CMAKE_CURRENT_SOURCE_DIR}/bridges/lean4/rules.json" --check
                       -o "${CMAKE_CURRENT_SOURCE_DIR}/include/simmynet/verified/LeanVerifiedRules.hpp")
    endif()
  endif()
endif()

# ----------------------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------------------
if(SIMMYNET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------------------
# Install headers + exported CMake target for downstream find_package / link users
# ----------------------------------------------------------------------------------------
install(DIRECTORY include/simmynet DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS simmynet_native_cpp EXPORT SimmyNetTargets)
install(EXPORT SimmyNetTargets
        NAMESPACE SimmyNet::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimmyNet)

message(STATUS "SimmyNet ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE}, native arch=${_simmynet_use_native}, "
               "CLI=${SIMMYNET_BUILD_CLI}, Python=${SIMMYNET_BUILD_PYTHON}, tests=${SIMMYNET_BUILD_TESTS}, "
               "static runtime=${SIMMYNET_STATIC_RUNTIME}, Lean FFI=${SIMMYNET_BUILD_LEAN_FFI}")
