#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project.hpp"
#include "luau/compiler.hpp"
#include "luau/environment.hpp"
#include "luau/module.hpp"
#include "luau/vm.hpp"

namespace sonata {

/*
 * Runtime ties a Project together with everything under core/luau/: one VM,
 * one Environment, and one ModuleLoader wired up with require() and the
 * project's dependency aliases installed. It's what "sn run", "sn build" and
 * "sn testmodule" all need before they can execute a single line of Luau,
 * and it replaces the boilerplate each of them would otherwise repeat:
 *
 *     Runtime runtime = Runtime::forPath(target);
 *     runtime.environment().setFunction("log", &myLog);
 *     runtime.runEntrypoint();
 *
 * LIFETIME
 *
 * ModuleLoader's own contract says it must be declared before the VM it's
 * installed on, so its destructor runs after the VM's. Runtime's members
 * are ordered to guarantee that internally; callers don't need to think
 * about it. Don't reach into vm()/loader() and store references that
 * outlive the Runtime.
 *
 * MOVING
 *
 * Runtime is move-only: it holds a VM, which can't be copied or moved
 * itself (only pointed to), so Runtime keeps it behind a unique_ptr and is
 * move-only as a result. This also means Runtime::forPath() is safe to
 * return by value with no extra cost.
 */
class Runtime {
public:
    /*
     * Wraps an already-open project. Wires up a ModuleLoader (backed by
     * FileSystemSource unless "source" is given), aliases every dependency
     * in the manifest that actually exists under sonata/deps, and installs
     * require() on a fresh VM. Manifest dependencies that don't exist on
     * disk are recorded in missingDependencies() instead of being reported
     * directly -- how (or whether) to warn about them is up to the caller.
     *
     * The Environment starts empty; populate it via environment() before
     * calling run()/runEntrypoint(), which push it onto the VM for you.
     */
    explicit Runtime(
        Project project,
        std::unique_ptr<luau::ModuleSource> source = nullptr
    );

    /*
     * Locates a project starting from "start" (Project::find) and builds a
     * Runtime for it in one step -- the common case for any command that
     * takes an optional [path] argument.
     */
    static Runtime forPath(
        const std::filesystem::path& start,
        std::unique_ptr<luau::ModuleSource> source = nullptr
    );

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = default;
    Runtime& operator=(Runtime&&) = default;

    [[nodiscard]] const Project& project() const noexcept;
    [[nodiscard]] luau::VM& vm() noexcept;
    [[nodiscard]] luau::Environment& environment() noexcept;
    [[nodiscard]] luau::ModuleLoader& loader() noexcept;

    /* Manifest dependencies that were skipped because sonata/deps/<name>
       doesn't exist. Empty if the project has no dependencies directory. */
    [[nodiscard]] const std::vector<std::string>& missingDependencies() const noexcept;

    /*
     * Re-pushes every global currently in environment() onto the VM.
     * run()/runEntrypoint() already do this before executing anything, so
     * you only need to call it yourself if you're driving the VM directly
     * (vm().execute(...)) without going through run().
     */
    void reload();

    /*
     * Compiles "source" on its own, outside the module system. Useful for
     * reporting a syntax error before require() gets involved, the way
     * "sn testmodule" does (see debugRun() below for that flow ready-made).
     */
    [[nodiscard]] luau::Bytecode compile(std::string_view source) const;

    /*
     * Runs "modulePath" (an absolute module path, forward-slashed) through
     * the module system exactly as require() would see it: relative
     * requires and @-aliases resolve normally. Reloads the environment
     * first. Throws luau::ModuleError on failure.
     */
    void run(std::string_view modulePath);

    /* Runs the project's own entrypoint. */
    void runEntrypoint();

    /*
     * Resolves the common "sn <command> [path]" argument: "target" names a
     * specific script if it's a regular file on disk, otherwise the
     * project's entrypoint is used. Call this after locating the project
     * (e.g. via forPath(target), which searches target's parent directory
     * the same way Project::find() does for a file argument).
     */
    [[nodiscard]] std::filesystem::path resolveScript(
        const std::filesystem::path& target
    ) const;

    /* Reads a script file into a string. Throws std::runtime_error if it
       can't be opened. */
    [[nodiscard]] static std::string readSource(const std::filesystem::path& path);

    struct RunResult {
        luau::Bytecode bytecode;
        long long compileMicros = 0;
        long long runMicros = 0;
    };

    /*
     * The two-step debug flow "sn testmodule" runs by hand: read
     * "scriptPath", compile it standalone first (so a syntax error is
     * reported clearly without require() involved), then run it for real
     * through the module system. Returns the bytecode and timing for both
     * steps; throws the same exceptions compile()/run() would, so callers
     * report errors exactly as before.
     */
    RunResult debugRun(const std::filesystem::path& scriptPath);

private:
    Project project_;
    std::unique_ptr<luau::ModuleLoader> loader_;
    std::unique_ptr<luau::VM> vm_;
    luau::Environment environment_;
    std::vector<std::string> missingDependencies_;
};

} // namespace sonata