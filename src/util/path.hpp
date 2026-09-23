#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace util {

namespace fs = std::filesystem;

using Path = fs::path;

// -----------------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------------

// Parses a single positional path argument.
// Returns std::nullopt if no path was supplied or a path was already specified.
// Throws no exceptions.
std::optional<fs::path> parse_path_arg(
    std::string_view arg,
    bool& pathspecified
);

// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------

// Checks whether a path can safely be treated as a directory path.
//
// This checks every existing component of the path. If an existing
// component is a regular file, the path is invalid.
//
// Non-existent components are allowed.
bool validate_directory_path(
    const fs::path& path,
    std::string& error
);

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

Path join(std::string_view a, std::string_view b);
Path join(
    std::string_view a,
    std::string_view b,
    std::string_view c
);

Path absolute(std::string_view path);
Path normalize(std::string_view path);

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------

bool exists(std::string_view path);
bool is_file(std::string_view path);
bool is_directory(std::string_view path);
bool is_symlink(std::string_view path);
bool is_empty(std::string_view path);

std::uintmax_t file_size(std::string_view path);

// -----------------------------------------------------------------------------
// Components
// -----------------------------------------------------------------------------

std::string filename(std::string_view path);
std::string stem(std::string_view path);
std::string extension(std::string_view path);
std::string parent(std::string_view path);

// -----------------------------------------------------------------------------
// Filesystem operations
// -----------------------------------------------------------------------------

bool create_directory(std::string_view path);
bool create_directories(std::string_view path);

bool remove(std::string_view path);
std::uintmax_t remove_all(std::string_view path);

bool copy(
    std::string_view from,
    std::string_view to,
    bool recursive = false
);

bool rename(
    std::string_view from,
    std::string_view to
);

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

std::string current_directory();
bool set_current_directory(std::string_view path);

std::vector<std::string> list_directory(std::string_view path);

} // namespace path