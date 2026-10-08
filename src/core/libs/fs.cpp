// @sonata/fs: Filesystem access
//
//     local fs = require("@sonata/fs")
//
// Every function raises a Lua error on failure (use pcall to catch it), except
// the exists / isFile / isDir / isSymlink queries, which just answer false.
// Paths are UTF-8 strings. File contents are binary-safe Lua strings.
//
// Variables
//     fs.version                             --> 1
//     fs.maxReadSize                         --> largest file readFile() accepts, in bytes
//     fs.modes                               --> permission presets for chmod():
//         .file 420 (0644)      .executable 493 (0755)   .directory 493 (0755)
//         .private 384 (0600)   .privateDirectory 448 (0700)   .readOnly 292 (0444)
//
// Files
//     fs.readFile(path)                      --> contents: string
//     fs.writeFile(path, contents)           creates or truncates
//     fs.appendFile(path, contents)          creates if missing
//     fs.copyFile(from, to, overwrite?)      overwrite defaults to true
//     fs.removeFile(path)
//     fs.move(from, to)                      rename; copies + deletes across devices
//
// Directories
//     fs.makeDir(path, recursive?)           non-recursive fails if it already exists
//     fs.removeDir(path, recursive?)         non-recursive fails if it is not empty
//     fs.listDir(path, recursive?)           --> { "a.txt", "sub" }  (sorted)
//     fs.readDir(path, recursive?)           --> { { name = "a.txt", kind = "file" }, ... }
//     fs.copyDir(from, to, overwrite?)       merges into `to`; overwrite defaults to true
//
//     With recursive = true, listDir / readDir return paths relative to `path`
//     with "/" separators ("sub/a.txt"). Symlinks are reported, not followed.
//
// Queries
//     fs.exists(path)                        --> boolean (follows symlinks)
//     fs.isFile(path), fs.isDir(path)        --> boolean (follow symlinks)
//     fs.isSymlink(path)                     --> boolean
//     fs.stat(path), fs.lstat(path)          --> { kind = "file" | "directory" | "symlink" | "other",
//                                                  size = bytes, modified = unix seconds, mode = number }
//                                                (lstat does not follow symlinks; size is 0 for
//                                                 anything but regular files)
//     fs.realPath(path)                      --> absolute path with symlinks resolved (must exist)
//
// Links and permissions
//     fs.symlink(target, link)
//     fs.readLink(path)                      --> target: string
//     fs.chmod(path, mode)                   e.g. fs.chmod(p, fs.modes.executable)
//
// Process-level locations
//     fs.cwd()                               --> string
//     fs.chdir(path)
//     fs.tempDir()                           --> the system temp directory
//     fs.makeTempDir(prefix?)                --> a new, empty, private directory inside it

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if __has_include(<version>)
#include <version>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

namespace stdfs = std::filesystem;

// Rule of thumb (same as example.cpp): validate ALL arguments first, then
// create C++ objects. Failures that only show up while the work is being done
// are not raised from inside the functions that own C++ objects. They push
// their message and return kFailed; finish() raises it once those functions
// have returned and their destructors have run.
//
//     int fsThing(lua_State* L) {
//         const std::string_view path = checkPath(L, 1);   // may raise: no objects yet
//         return finish(L, thingImpl(L, path));            // impl never raises
//     }

constexpr int kFailed = -1;

// Luau strings are limited to 1 GiB.
constexpr std::uintmax_t kMaxReadSize = (std::uintmax_t{1} << 30) - 1;

int fail(lua_State* L, const char* op, const std::string& subject, std::string_view reason) {
    std::string message = "fs.";
    message += op;
    if (!subject.empty()) {
        message += " ";
        message += subject;
    }
    message += ": ";
    message.append(reason);
    lua_pushlstring(L, message.data(), message.size());
    return kFailed;
}

int finish(lua_State* L, int results) {
    if (results < 0) {
        luaL_error(L, "%s", lua_tostring(L, -1));
    }
    return results;
}

std::string quote(std::string_view path) {
    std::string out = "'";
    out.append(path);
    out += "'";
    return out;
}

std::string quote(std::string_view from, std::string_view to) {
    return quote(from) + " -> " + quote(to);
}

std::string reasonOf(std::errc code) {
    return std::make_error_code(code).message();
}

std::string errnoReason(int error) {
    if (error == 0) {
        return "operation failed";
    }
    return std::error_code(error, std::generic_category()).message();
}

std::string_view checkPath(lua_State* L, int arg) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, arg, &length);
    luaL_argcheck(L, std::strlen(text) == length, arg, "path must not contain NUL bytes");
    return std::string_view(text, length);
}

std::string_view checkData(lua_State* L, int arg) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, arg, &length);
    return std::string_view(text, length);
}

bool optBoolean(lua_State* L, int arg, bool fallback) {
    if (lua_isnoneornil(L, arg)) {
        return fallback;
    }
    luaL_checktype(L, arg, LUA_TBOOLEAN);
    return lua_toboolean(L, arg) != 0;
}

void pushString(lua_State* L, std::string_view text) {
    lua_pushlstring(L, text.data(), text.size());
}

void setNumberField(lua_State* L, const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void setStringField(lua_State* L, const char* key, std::string_view text) {
    pushString(L, text);
    lua_setfield(L, -2, key);
}

// UTF-8 <-> std::filesystem::path (std::string would be the ANSI code page on
// Windows)
////////////////////////////////////////////////

stdfs::path toPath(std::string_view text) {
#if defined(__cpp_lib_char8_t)
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
    return stdfs::u8path(text.begin(), text.end());
#endif
}

std::string toUtf8(const stdfs::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string text = p.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return p.u8string();
#endif
}

// Same, but with "/" as the separator on every platform.
std::string toGenericUtf8(const stdfs::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string text = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    return p.generic_u8string();
#endif
}

const char* kindOf(const stdfs::file_status& status) {
    switch (status.type()) {
    case stdfs::file_type::regular:
        return "file";
    case stdfs::file_type::directory:
        return "directory";
    case stdfs::file_type::symlink:
        return "symlink";
    default:
        return "other";
    }
}

double toUnixSeconds(stdfs::file_time_type time) {
    // C++17 has no portable file_clock -> system_clock conversion; measure the
    // distance to "now" on both clocks instead (accurate to microseconds).
    const auto delta = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        time - stdfs::file_time_type::clock::now());
    const auto system = std::chrono::system_clock::now() + delta;
    return std::chrono::duration<double>(system.time_since_epoch()).count();
}

// Files ////////////////////////

int readFileImpl(lua_State* L, std::string_view pathText) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status = stdfs::status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "readFile", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (stdfs::is_directory(status)) {
        return fail(L, "readFile", quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(L, "readFile", quote(pathText), errnoReason(errno));
    }

    // Do not trust the size alone: pipes and /proc files report 0.
    std::string data;
    if (stdfs::is_regular_file(status)) {
        const std::uintmax_t size = stdfs::file_size(path, ec);
        if (!ec) {
            if (size > kMaxReadSize) {
                return fail(L, "readFile", quote(pathText), "file is too large to read into a string");
            }
            data.reserve(static_cast<std::size_t>(size));
        }
    }

    std::vector<char> chunk(1 << 16);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        data.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
        if (data.size() > kMaxReadSize) {
            return fail(L, "readFile", quote(pathText), "file is too large to read into a string");
        }
    }
    if (in.bad()) {
        return fail(L, "readFile", quote(pathText), "read error");
    }

    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int writeImpl(lua_State* L, const char* op, std::string_view pathText, std::string_view data,
              bool append) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (stdfs::is_directory(path, ec)) {
        return fail(L, op, quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        return fail(L, op, quote(pathText), errnoReason(errno));
    }

    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (out.fail()) {
        return fail(L, op, quote(pathText), "write error");
    }
    return 0;
}

int copyFileImpl(lua_State* L, std::string_view fromText, std::string_view toText, bool overwrite) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    stdfs::copy_file(from, to,
                     overwrite ? stdfs::copy_options::overwrite_existing : stdfs::copy_options::none,
                     ec);
    if (ec) {
        return fail(L, "copyFile", quote(fromText, toText), ec.message());
    }
    return 0;
}

int removeFileImpl(lua_State* L, std::string_view pathText) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "removeFile", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, "removeFile", quote(pathText), ec.message());
    }
    if (stdfs::is_directory(status)) {
        return fail(L, "removeFile", quote(pathText), reasonOf(std::errc::is_a_directory));
    }

    stdfs::remove(path, ec);
    if (ec) {
        return fail(L, "removeFile", quote(pathText), ec.message());
    }
    return 0;
}

int moveImpl(lua_State* L, std::string_view fromText, std::string_view toText) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    stdfs::rename(from, to, ec);
    if (!ec) {
        return 0;
    }
    if (ec != std::errc::cross_device_link) {
        return fail(L, "move", quote(fromText, toText), ec.message());
    }

    // Different filesystems: copy, then delete the source.
    std::error_code ignored;
    if (stdfs::is_directory(stdfs::symlink_status(to, ignored))) {
        return fail(L, "move", quote(fromText, toText), "destination is a directory");
    }
    stdfs::copy(from, to,
                stdfs::copy_options::recursive | stdfs::copy_options::copy_symlinks |
                    stdfs::copy_options::overwrite_existing,
                ec);
    if (ec) {
        return fail(L, "move", quote(fromText, toText), ec.message());
    }
    stdfs::remove_all(from, ec);
    if (ec) {
        return fail(L, "move", quote(fromText, toText), ec.message());
    }
    return 0;
}

// Directories ////////////////////

int makeDirImpl(lua_State* L, std::string_view pathText, bool recursive) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (recursive) {
        stdfs::create_directories(path, ec);
        if (!ec && !stdfs::is_directory(path, ec)) {
            return fail(L, "makeDir", quote(pathText), reasonOf(std::errc::not_a_directory));
        }
        if (ec) {
            return fail(L, "makeDir", quote(pathText), ec.message());
        }
        return 0;
    }

    const bool created = stdfs::create_directory(path, ec);
    if (ec) {
        return fail(L, "makeDir", quote(pathText), ec.message());
    }
    if (!created) {
        return fail(L, "makeDir", quote(pathText), reasonOf(std::errc::file_exists));
    }
    return 0;
}

int removeDirImpl(lua_State* L, std::string_view pathText, bool recursive) {
    const stdfs::path path = toPath(pathText);

    // Never delete a filesystem root, whatever the flags say.
    if (!path.has_relative_path()) {
        return fail(L, "removeDir", quote(pathText), "refusing to remove a filesystem root");
    }

    std::error_code ec;
    const stdfs::file_status status = stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, "removeDir", quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, "removeDir", quote(pathText), ec.message());
    }
    // A symlink to a directory is not a directory here: it is never followed.
    if (!stdfs::is_directory(status)) {
        return fail(L, "removeDir", quote(pathText), reasonOf(std::errc::not_a_directory));
    }

    if (recursive) {
        stdfs::remove_all(path, ec);
    } else {
        stdfs::remove(path, ec);
    }
    if (ec) {
        return fail(L, "removeDir", quote(pathText), ec.message());
    }
    return 0;
}

struct Entry {
    std::string name;
    const char* kind;
};

template <typename Iterator>
bool scanEntries(const stdfs::path& dir, bool relativeToDir, std::vector<Entry>& out,
                 std::error_code& ec) {
    for (Iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const stdfs::directory_entry& entry = *it;

        std::error_code statusEc;
        const stdfs::file_status status = entry.symlink_status(statusEc);

        Entry item;
        item.name = relativeToDir ? toGenericUtf8(entry.path().lexically_relative(dir))
                                  : toUtf8(entry.path().filename());
        item.kind = statusEc ? "other" : kindOf(status);
        out.push_back(std::move(item));
    }
    return !ec;
}

// Collects the entries of a directory, sorted by name. Returns false with ec set on failure.
bool collectEntries(const stdfs::path& dir, bool recursive, std::vector<Entry>& out,
                    std::error_code& ec) {
    const bool ok = recursive ? scanEntries<stdfs::recursive_directory_iterator>(dir, true, out, ec)
                              : scanEntries<stdfs::directory_iterator>(dir, false, out, ec);
    if (ok) {
        std::sort(out.begin(), out.end(),
                  [](const Entry& a, const Entry& b) { return a.name < b.name; });
    }
    return ok;
}

int listDirImpl(lua_State* L, const char* op, std::string_view pathText, bool recursive,
                bool withKinds) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    if (!stdfs::is_directory(path, ec)) {
        return fail(L, op, quote(pathText),
                    stdfs::exists(path, ec) ? reasonOf(std::errc::not_a_directory)
                                            : reasonOf(std::errc::no_such_file_or_directory));
    }

    std::vector<Entry> entries;
    if (!collectEntries(path, recursive, entries, ec)) {
        return fail(L, op, quote(pathText), ec.message());
    }

    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (withKinds) {
            lua_createtable(L, 0, 2);
            setStringField(L, "name", entries[i].name);
            setStringField(L, "kind", entries[i].kind);
        } else {
            pushString(L, entries[i].name);
        }
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int copyDirImpl(lua_State* L, std::string_view fromText, std::string_view toText, bool overwrite) {
    const stdfs::path from = toPath(fromText);
    const stdfs::path to = toPath(toText);

    std::error_code ec;
    if (!stdfs::is_directory(from, ec)) {
        return fail(L, "copyDir", quote(fromText, toText), "source is not a directory");
    }

    // Copying a directory into itself would never end.
    const stdfs::path source = stdfs::weakly_canonical(from, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), ec.message());
    }
    const stdfs::path target = stdfs::weakly_canonical(to, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), ec.message());
    }
    const stdfs::path relation = target.lexically_relative(source);
    if (!relation.empty() && *relation.begin() != stdfs::path("..")) {
        return fail(L, "copyDir", quote(fromText, toText),
                    "destination is the source directory or inside it");
    }

    stdfs::copy_options options = stdfs::copy_options::recursive | stdfs::copy_options::copy_symlinks;
    if (overwrite) {
        options |= stdfs::copy_options::overwrite_existing;
    }
    stdfs::copy(from, to, options, ec);
    if (ec) {
        return fail(L, "copyDir", quote(fromText, toText), ec.message());
    }
    return 0;
}

int tempDirImpl(lua_State* L) {
    std::error_code ec;
    const stdfs::path dir = stdfs::temp_directory_path(ec);
    if (ec) {
        return fail(L, "tempDir", "", ec.message());
    }
    pushString(L, toUtf8(dir));
    return 1;
}

int makeTempDirImpl(lua_State* L, std::string_view prefix) {
    std::error_code ec;
    const stdfs::path base = stdfs::temp_directory_path(ec);
    if (ec) {
        return fail(L, "makeTempDir", "", ec.message());
    }

    static const char kDigits[] = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 random(
        (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device()));

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name(prefix);
        for (int i = 0; i < 12; ++i) {
            name.push_back(kDigits[random() & 0xf]);
        }

        const stdfs::path candidate = base / toPath(name);
        if (stdfs::create_directory(candidate, ec)) {
            stdfs::permissions(candidate, stdfs::perms::owner_all, ec); // best effort: 0700
            pushString(L, toUtf8(candidate));
            return 1;
        }
        if (ec) {
            return fail(L, "makeTempDir", quote(toUtf8(candidate)), ec.message());
        }
        // The name was taken: try another one.
    }
    return fail(L, "makeTempDir", quote(toUtf8(base)), "could not find an unused name");
}

int cwdImpl(lua_State* L) {
    std::error_code ec;
    const stdfs::path dir = stdfs::current_path(ec);
    if (ec) {
        return fail(L, "cwd", "", ec.message());
    }
    pushString(L, toUtf8(dir));
    return 1;
}

int chdirImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    stdfs::current_path(toPath(pathText), ec);
    if (ec) {
        return fail(L, "chdir", quote(pathText), ec.message());
    }
    return 0;
}

// Queries, links, permissions ///////////////

int statImpl(lua_State* L, const char* op, std::string_view pathText, bool follow) {
    const stdfs::path path = toPath(pathText);

    std::error_code ec;
    const stdfs::file_status status =
        follow ? stdfs::status(path, ec) : stdfs::symlink_status(path, ec);
    if (status.type() == stdfs::file_type::not_found) {
        return fail(L, op, quote(pathText), reasonOf(std::errc::no_such_file_or_directory));
    }
    if (ec) {
        return fail(L, op, quote(pathText), ec.message());
    }

    double size = 0;
    if (stdfs::is_regular_file(status)) {
        std::error_code sizeEc;
        const std::uintmax_t bytes = stdfs::file_size(path, sizeEc);
        if (!sizeEc) {
            size = static_cast<double>(bytes);
        }
    }

    double modified = 0;
    std::error_code timeEc;
    const stdfs::file_time_type time = stdfs::last_write_time(path, timeEc);
    if (!timeEc) {
        modified = toUnixSeconds(time);
    }

    lua_createtable(L, 0, 4);
    setStringField(L, "kind", kindOf(status));
    setNumberField(L, "size", size);
    setNumberField(L, "modified", modified);
    setNumberField(L, "mode", static_cast<double>(static_cast<unsigned>(status.permissions() &
                                                                         stdfs::perms::mask)));
    return 1;
}

int realPathImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    const stdfs::path resolved = stdfs::canonical(toPath(pathText), ec);
    if (ec) {
        return fail(L, "realPath", quote(pathText), ec.message());
    }
    pushString(L, toUtf8(resolved));
    return 1;
}

int symlinkImpl(lua_State* L, std::string_view targetText, std::string_view linkText) {
    const stdfs::path target = toPath(targetText);
    const stdfs::path link = toPath(linkText);

    // Windows distinguishes links to directories from links to files.
    std::error_code ec;
    const stdfs::path anchored = target.is_absolute() ? target : link.parent_path() / target;
    const bool toDirectory = stdfs::is_directory(anchored, ec);

    if (toDirectory) {
        stdfs::create_directory_symlink(target, link, ec);
    } else {
        stdfs::create_symlink(target, link, ec);
    }
    if (ec) {
        return fail(L, "symlink", quote(linkText, targetText), ec.message());
    }
    return 0;
}

int readLinkImpl(lua_State* L, std::string_view pathText) {
    std::error_code ec;
    const stdfs::path target = stdfs::read_symlink(toPath(pathText), ec);
    if (ec) {
        return fail(L, "readLink", quote(pathText), ec.message());
    }
    pushString(L, toUtf8(target));
    return 1;
}

int chmodImpl(lua_State* L, std::string_view pathText, int mode) {
    std::error_code ec;
    stdfs::permissions(toPath(pathText), static_cast<stdfs::perms>(mode), ec);
    if (ec) {
        return fail(L, "chmod", quote(pathText), ec.message());
    }
    return 0;
}

// Bindings /////////////////////////////////

// fs.readFile(path: string): string
int fsReadFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, readFileImpl(L, path));
}

// fs.writeFile(path: string, contents: string)
int fsWriteFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const std::string_view data = checkData(L, 2);
    return finish(L, writeImpl(L, "writeFile", path, data, false));
}

// fs.appendFile(path: string, contents: string)
int fsAppendFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const std::string_view data = checkData(L, 2);
    return finish(L, writeImpl(L, "appendFile", path, data, true));
}

// fs.copyFile(from: string, to: string, overwrite: boolean?)
int fsCopyFile(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    const bool overwrite = optBoolean(L, 3, true);
    return finish(L, copyFileImpl(L, from, to, overwrite));
}

// fs.removeFile(path: string)
int fsRemoveFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, removeFileImpl(L, path));
}

// fs.move(from: string, to: string)
int fsMove(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    return finish(L, moveImpl(L, from, to));
}

// fs.makeDir(path: string, recursive: boolean?)
int fsMakeDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, makeDirImpl(L, path, recursive));
}

// fs.removeDir(path: string, recursive: boolean?)
int fsRemoveDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, removeDirImpl(L, path, recursive));
}

// fs.listDir(path: string, recursive: boolean?): { string }
int fsListDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, listDirImpl(L, "listDir", path, recursive, false));
}

// fs.readDir(path: string, recursive: boolean?): { { name: string, kind: string } }
int fsReadDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const bool recursive = optBoolean(L, 2, false);
    return finish(L, listDirImpl(L, "readDir", path, recursive, true));
}

// fs.copyDir(from: string, to: string, overwrite: boolean?)
int fsCopyDir(lua_State* L) {
    const std::string_view from = checkPath(L, 1);
    const std::string_view to = checkPath(L, 2);
    const bool overwrite = optBoolean(L, 3, true);
    return finish(L, copyDirImpl(L, from, to, overwrite));
}

// fs.exists(path: string): boolean
int fsExists(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::exists(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isFile(path: string): boolean
int fsIsFile(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_regular_file(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isDir(path: string): boolean
int fsIsDir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_directory(stdfs::status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.isSymlink(path: string): boolean
int fsIsSymlink(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    std::error_code ec;
    lua_pushboolean(L, stdfs::is_symlink(stdfs::symlink_status(toPath(path), ec)) ? 1 : 0);
    return 1;
}

// fs.stat(path: string): { kind: string, size: number, modified: number, mode: number }
int fsStat(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, statImpl(L, "stat", path, true));
}

// fs.lstat(path: string): { kind: string, size: number, modified: number, mode: number }
int fsLstat(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, statImpl(L, "lstat", path, false));
}

// fs.realPath(path: string): string
int fsRealPath(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, realPathImpl(L, path));
}

// fs.symlink(target: string, link: string)
int fsSymlink(lua_State* L) {
    const std::string_view target = checkPath(L, 1);
    const std::string_view link = checkPath(L, 2);
    return finish(L, symlinkImpl(L, target, link));
}

// fs.readLink(path: string): string
int fsReadLink(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, readLinkImpl(L, path));
}

// fs.chmod(path: string, mode: number)
int fsChmod(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    const int mode = luaL_checkinteger(L, 2);
    luaL_argcheck(L, mode >= 0 && mode <= 4095, 2, "mode must be an integer between 0 and 4095");
    return finish(L, chmodImpl(L, path, mode));
}

// fs.cwd(): string
int fsCwd(lua_State* L) {
    return finish(L, cwdImpl(L));
}

// fs.chdir(path: string)
int fsChdir(lua_State* L) {
    const std::string_view path = checkPath(L, 1);
    return finish(L, chdirImpl(L, path));
}

// fs.tempDir(): string
int fsTempDir(lua_State* L) {
    return finish(L, tempDirImpl(L));
}

// fs.makeTempDir(prefix: string?): string
int fsMakeTempDir(lua_State* L) {
    std::size_t length = 0;
    const char* prefix = luaL_optlstring(L, 1, "sonata-", &length);
    const std::string_view text(prefix, length);
    luaL_argcheck(L, text.find_first_of("/\\") == std::string_view::npos &&
                         std::strlen(prefix) == length,
                  1, "prefix must not contain path separators or NUL bytes");
    return finish(L, makeTempDirImpl(L, text));
}

constexpr NativeFunction kFunctions[] = {
    {"readFile", fsReadFile},
    {"writeFile", fsWriteFile},
    {"appendFile", fsAppendFile},
    {"copyFile", fsCopyFile},
    {"removeFile", fsRemoveFile},
    {"move", fsMove},
    {"makeDir", fsMakeDir},
    {"removeDir", fsRemoveDir},
    {"listDir", fsListDir},
    {"readDir", fsReadDir},
    {"copyDir", fsCopyDir},
    {"exists", fsExists},
    {"isFile", fsIsFile},
    {"isDir", fsIsDir},
    {"isSymlink", fsIsSymlink},
    {"stat", fsStat},
    {"lstat", fsLstat},
    {"realPath", fsRealPath},
    {"symlink", fsSymlink},
    {"readLink", fsReadLink},
    {"chmod", fsChmod},
    {"cwd", fsCwd},
    {"chdir", fsChdir},
    {"tempDir", fsTempDir},
    {"makeTempDir", fsMakeTempDir},
};

} // namespace


void openFs(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 3);
    setFunctions(L, kFunctions);

    lua_pushnumber(L, 1);
    lua_setfield(L, -2, "version");

    lua_pushnumber(L, static_cast<double>(kMaxReadSize));
    lua_setfield(L, -2, "maxReadSize");

    lua_createtable(L, 0, 6);
    setNumberField(L, "file", 0644);
    setNumberField(L, "executable", 0755);
    setNumberField(L, "directory", 0755);
    setNumberField(L, "private", 0600);
    setNumberField(L, "privateDirectory", 0700);
    setNumberField(L, "readOnly", 0444);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "modes");
}

} // namespace sonata::lib::libs