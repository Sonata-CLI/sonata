#pragma once

// Single source of truth for Sonata's identity and build metadata.
//
// Every value can be overridden from the build system, e.g. in CMake:
//
//   target_compile_definitions(sonata PRIVATE
//       SONATA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
//       SONATA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
//       SONATA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
//       SONATA_GIT_HASH="${GIT_HASH}"
//       SONATA_LUAU_VERSION="${LUAU_VERSION}")
//
// Anything left undefined falls back to the defaults below.

#include <string>
#include <string_view>

#ifndef SONATA_VERSION_MAJOR
#define SONATA_VERSION_MAJOR 0
#endif
#ifndef SONATA_VERSION_MINOR
#define SONATA_VERSION_MINOR 1
#endif
#ifndef SONATA_VERSION_PATCH
#define SONATA_VERSION_PATCH 0
#endif
#ifndef SONATA_VERSION_PRE // pre-release tag, e.g. "alpha.1" (leave empty for stable)
#define SONATA_VERSION_PRE ""
#endif
#ifndef SONATA_GIT_HASH
#define SONATA_GIT_HASH ""
#endif
#ifndef SONATA_LUAU_VERSION
#define SONATA_LUAU_VERSION ""
#endif
#ifndef SONATA_BUILD_TYPE
#ifdef NDEBUG
#define SONATA_BUILD_TYPE "release"
#else
#define SONATA_BUILD_TYPE "debug"
#endif
#endif

namespace sonata::info {

    inline constexpr std::string_view name = "Sonata";
    inline constexpr std::string_view binary = "sn";
    inline constexpr std::string_view tagline = "All-in-one Luau toolkit";

    struct Version {
        int major;
        int minor;
        int patch;
        std::string_view pre;
    };

    inline constexpr Version version {
        SONATA_VERSION_MAJOR,
        SONATA_VERSION_MINOR,
        SONATA_VERSION_PATCH,
        SONATA_VERSION_PRE
    };

    inline constexpr std::string_view git_hash = SONATA_GIT_HASH;
    inline constexpr std::string_view luau_version = SONATA_LUAU_VERSION;
    inline constexpr std::string_view build_type = SONATA_BUILD_TYPE;

    // "0.1.0" or "0.1.0-alpha.1"
    inline std::string version_string() {
        std::string out = std::to_string(version.major) + '.' +
            std::to_string(version.minor) + '.' +
            std::to_string(version.patch);

        if (!version.pre.empty()) {
            out += '-';
            out += version.pre;
        }

        return out;
    }

    // "linux-x64", "macos-arm64", "windows-x64"
    inline std::string platform_string() {
#if defined(_WIN32)
        std::string os = "windows";
#elif defined(__APPLE__)
        std::string os = "macos";
#elif defined(__linux__)
        std::string os = "linux";
#else
        std::string os = "unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
        std::string arch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        std::string arch = "arm64";
#else
        std::string arch = "unknown";
#endif

        return os + '-' + arch;
    }

    // "gcc 13.2.0", "clang 17.0.6", "msvc 1939"
    inline std::string compiler_string() {
#if defined(__clang__)
        return "clang " + std::to_string(__clang_major__) + '.' +
            std::to_string(__clang_minor__) + '.' +
            std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
        return "msvc " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
        return "gcc " + std::to_string(__GNUC__) + '.' +
            std::to_string(__GNUC_MINOR__) + '.' +
            std::to_string(__GNUC_PATCHLEVEL__);
#else
        return "unknown";
#endif
    }

} // namespace sonata::info