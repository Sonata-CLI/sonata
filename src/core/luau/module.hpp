#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler.hpp"
#include "vm.hpp"

namespace sonata::luau {

// Raised for failures the module system detects itself: a module that can't be
// read or compiled, or an entry script that errors inside ModuleLoader::run().
class ModuleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/*
 * MODULE PATHS
 *
 * Every module is identified by a module path: an absolute path written with
 * forward slashes, e.g.
 *
 *     /home/me/project/src/util.luau
 *     C:/project/src/util.luau
 *
 * "sn run"   uses the real absolute paths on disk.
 * "sn build" uses the same tree under a virtual root: /main.luau,
 *            /src/util.luau, /sonata/deps/foo/init.luau, ...
 */

/*
 * Lexically normalises an absolute path: converts '\' to '/', drops "." and
 * empty segments, resolves "..".
 *
 * Returns std::nullopt if the path isn't absolute, or if ".." would climb above
 * its root.
 */
[[nodiscard]]
std::optional<std::string> normalizePath(std::string_view path);

/*
 * Where module bytecode comes from. The loader never touches the disk (or the
 * packed archive) directly, it only talks to one of these.
 *
 * Paths passed in are always normalised module paths (see above).
 */
class ModuleSource {
public:
    virtual ~ModuleSource() = default;

    /* True if a module file exists at exactly this path. */
    virtual bool exists(std::string_view path) const = 0;

    /* Returns the module's bytecode. Throws (ModuleError ideally) on failure. */
    virtual Bytecode load(std::string_view path) const = 0;
};

/*
 * sn run: reads .luau files from disk and compiles them on demand.
 */
class FileSystemSource final : public ModuleSource {
public:
    bool exists(std::string_view path) const override;
    Bytecode load(std::string_view path) const override;

private:
    Compiler compiler_;
};

/*
 * Precompiled modules held in memory, keyed by virtual path.
 *
 * "sn build" can populate one of these from the packed archive at startup, or
 * you can implement ModuleSource directly on top of your archive reader
 * instead. Handy for tests either way.
 */
class MemorySource final : public ModuleSource {
public:
    /* "path" is rooted at "/", so add("src/util.luau", ...) and
       add("/src/util.luau", ...) are equivalent. Throws std::invalid_argument
       if the path climbs above the root. */
    void add(std::string_view path, Bytecode bytecode);

    bool exists(std::string_view path) const override;
    Bytecode load(std::string_view path) const override;

private:
    std::unordered_map<std::string, Bytecode> files_;
};

/*
 * Implements "require" for one VM.
 *
 * Accepted require strings:
 *
 *     require("./helper")         relative to the requiring file's directory
 *     require("../shared/util")
 *     require("@name/path")       through an alias registered with addAlias()
 *     require("@name")
 *
 * A path P resolves to P.luau, or P/init.luau if that doesn't exist. If both
 * exist the require is an error. Anything not starting with "./", "../" or "@"
 * is rejected (this keeps bare names free for a future package mechanism).
 *
 * A module runs once per VM, on its own sandboxed thread, and must return
 * exactly one non-nil value. That value is cached and returned by every later
 * require of the same file. Cyclic requires are reported as errors.
 *
 * LIFETIME: install() gives the VM a raw pointer to this object, so the loader
 * must outlive every piece of Lua code that can run, and must not be moved.
 * Use one loader per VM, and declare it before the VM.
 */
class ModuleLoader {
public:
    explicit ModuleLoader(std::unique_ptr<ModuleSource> source);

    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;

    /*
     * Registers "@name" as an alias for a directory (a module path).
     * Throws std::invalid_argument if the name or path is invalid.
     *
     * For dependencies: addAlias("foo", ".../sonata/deps/foo").
     */
    void addAlias(std::string name, std::string_view directory);

    /*
     * Defines the global "require" on the VM. Call it before luaL_sandbox()
     * (or whatever makes your globals read-only).
     */
    void install(VM& vm);

    /*
     * Runs "entry" (a module path) as the program's entry point.
     *
     * The entry goes through the same machinery as required modules, so its
     * chunk name is its path and relative requires work from it. It may return
     * anything. Throws ModuleError, including a stack trace, if it fails.
     */
    void run(VM& vm, std::string_view entry);

private:
    static int requireCallback(lua_State* L);

    // Returns the number of results, or -1 with an error value on top of the stack.
    int requireModule(lua_State* L, std::string_view request);

    bool resolve(
        std::string_view request,
        const std::string& caller,
        std::string& path,
        std::string& error
    ) const;

    std::unique_ptr<ModuleSource> source_;
    std::unordered_map<std::string, std::string> aliases_;
    std::vector<std::string> loading_; // modules currently executing, outermost first
};

} // namespace sonata::luau