#include "path.hpp"

#include <system_error>

namespace util {

namespace {

fs::path make_path(std::string_view path)
{
    return fs::path(path);
}

} // namespace

// -----------------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------------

std::optional<fs::path> parse_path_arg(
    std::string_view arg,
    bool& pathspecified
) {
    if (arg.empty())
        return std::nullopt;

    if (pathspecified)
        return std::nullopt;

    pathspecified = true;

    return fs::path(arg);
}

// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------

bool validate_directory_path(
    const fs::path& path,
    std::string& error
) {
    if (path.empty()) {
        error = "path is empty";
        return false;
    }

    std::error_code ec;
    fs::path current;

    // Walk through each component instead of only checking
    // the final path.
    for (const auto& component : path) {
        current /= component;

        if (!fs::exists(current, ec)) {
            if (ec) {
                error = "could not inspect path '" +
                        current.string() + "': " +
                        ec.message();
                return false;
            }

            // This component doesn't exist. Everything after it
            // cannot be an existing file, so we're done checking.
            break;
        }

        if (!fs::is_directory(current, ec)) {
            if (ec) {
                error = "could not inspect path '" +
                        current.string() + "': " +
                        ec.message();
                return false;
            }

            error = "path component is not a directory: '" +
                    current.string() + "'";
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

Path join(std::string_view a, std::string_view b)
{
    return make_path(a) / make_path(b);
}

Path join(
    std::string_view a,
    std::string_view b,
    std::string_view c
) {
    return make_path(a) /
           make_path(b) /
           make_path(c);
}

Path absolute(std::string_view path)
{
    std::error_code ec;

    const auto result = fs::absolute(
        make_path(path),
        ec
    );

    if (ec)
        return {};

    return result;
}

Path normalize(std::string_view path)
{
    return make_path(path).lexically_normal();
}

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------

bool exists(std::string_view path)
{
    std::error_code ec;

    return fs::exists(
        make_path(path),
        ec
    );
}

bool is_file(std::string_view path)
{
    std::error_code ec;

    return fs::is_regular_file(
        make_path(path),
        ec
    );
}

bool is_directory(std::string_view path)
{
    std::error_code ec;

    return fs::is_directory(
        make_path(path),
        ec
    );
}

bool is_symlink(std::string_view path)
{
    std::error_code ec;

    return fs::is_symlink(
        make_path(path),
        ec
    );
}

bool is_empty(std::string_view path)
{
    std::error_code ec;

    return fs::is_empty(
        make_path(path),
        ec
    );
}

std::uintmax_t file_size(std::string_view path)
{
    std::error_code ec;

    const auto size = fs::file_size(
        make_path(path),
        ec
    );

    if (ec)
        return 0;

    return size;
}

// -----------------------------------------------------------------------------
// Components
// -----------------------------------------------------------------------------

std::string filename(std::string_view path)
{
    return make_path(path).filename().string();
}

std::string stem(std::string_view path)
{
    return make_path(path).stem().string();
}

std::string extension(std::string_view path)
{
    return make_path(path).extension().string();
}

std::string parent(std::string_view path)
{
    return make_path(path).parent_path().string();
}

// -----------------------------------------------------------------------------
// Filesystem operations
// -----------------------------------------------------------------------------

bool create_directory(std::string_view path)
{
    std::error_code ec;

    return fs::create_directory(
        make_path(path),
        ec
    );
}

bool create_directories(std::string_view path)
{
    std::error_code ec;
    const auto p = make_path(path);

    if (fs::create_directories(p, ec))
        return true;

    return !ec && fs::is_directory(p);
}

bool remove(std::string_view path)
{
    std::error_code ec;

    return fs::remove(
        make_path(path),
        ec
    );
}

std::uintmax_t remove_all(std::string_view path)
{
    std::error_code ec;

    return fs::remove_all(
        make_path(path),
        ec
    );
}

bool copy(
    std::string_view from,
    std::string_view to,
    bool recursive
) {
    std::error_code ec;

    auto options = fs::copy_options::none;

    if (recursive)
        options |= fs::copy_options::recursive;

    fs::copy(
        make_path(from),
        make_path(to),
        options,
        ec
    );

    return !ec;
}

bool rename(
    std::string_view from,
    std::string_view to
) {
    std::error_code ec;

    fs::rename(
        make_path(from),
        make_path(to),
        ec
    );

    return !ec;
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

std::string current_directory()
{
    std::error_code ec;

    const auto path = fs::current_path(ec);

    if (ec)
        return {};

    return path.string();
}

bool set_current_directory(std::string_view path)
{
    std::error_code ec;

    fs::current_path(
        make_path(path),
        ec
    );

    return !ec;
}

std::vector<std::string> list_directory(std::string_view path)
{
    std::vector<std::string> result;

    std::error_code ec;

    const auto directory = make_path(path);

    fs::directory_iterator iterator(
        directory,
        fs::directory_options::skip_permission_denied,
        ec
    );

    if (ec)
        return result;

    for (const auto& entry : iterator)
        result.emplace_back(entry.path().string());

    return result;
}

} // namespace path