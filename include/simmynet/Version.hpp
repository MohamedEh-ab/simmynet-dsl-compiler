// Version.hpp - the single C++ source of truth for the SimmyNet version.
//
// Keep in sync with: CMakeLists.txt (project VERSION), pyproject.toml ([project] version) and
// CITATION.cff.  CMake refuses to configure if this header and project(VERSION) disagree, and
// tests/python/test_bindings.py checks the Python `__version__` against pyproject.toml.
#pragma once

#define SIMMYNET_VERSION_MAJOR 2
#define SIMMYNET_VERSION_MINOR 2
#define SIMMYNET_VERSION_PATCH 0
#define SIMMYNET_VERSION_STRING "2.2.0"
// major * 10000 + minor * 100 + patch, for #if comparisons (2.2.0 -> 20200).
#define SIMMYNET_VERSION_NUMBER (SIMMYNET_VERSION_MAJOR * 10000 + SIMMYNET_VERSION_MINOR * 100 + SIMMYNET_VERSION_PATCH)

namespace simmynet {

inline constexpr int kVersionMajor = SIMMYNET_VERSION_MAJOR;
inline constexpr int kVersionMinor = SIMMYNET_VERSION_MINOR;
inline constexpr int kVersionPatch = SIMMYNET_VERSION_PATCH;
inline constexpr const char* kVersionString = SIMMYNET_VERSION_STRING;

}  // namespace simmynet
