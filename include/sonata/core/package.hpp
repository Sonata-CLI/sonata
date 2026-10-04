#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sonata/core/project.hpp>

namespace sonata {

namespace luau {
class ModuleLoader;
}

/*
 * PACKAGE MANAGER
 *
 * Dependencies are plain git repositories that are themselves Sonata projects
 * (they must contain .sonata/project.luau and the entrypoint it names).
 * They are declared in the root project's manifest:
 *
 *     dependencies = {
 *         json  = "https://github.com/someone/json.git",
 *         utils = "https://github.com/someone/utils.git#v1.2.0",
 *     }
 *
 * "#<ref>" is optional and may be a branch, a tag or a commit hash. Without
 * it the remote's default branch is used.
 *
 * Layout:
 *
 *     project/
 *     └── .sonata/
 *         ├── project.luau
 *         ├── lock.luau        what was installed (url, ref, exact commit)
 *         └── deps/
 *             ├── json/        a dependency (checked out, without .git)
 *             ├── utils/
 *             └── .staging/    scratch space, always cleaned up
 *
 * Everything lives flat in the ROOT project's .sonata/deps, including
 * dependencies of dependencies. If two packages ask for the same name from
 * different sources, the root manifest wins; otherwise it is an error.
 * Everything inside .sonata/deps is owned by the package manager.
 *
 * HOW A PACKAGE IS INSTALLED
 *
 *   1. Clone it into .sonata/deps/.staging/<random>.
 *      The "git" executable is used first; if it is missing or fails,
 *      libgit2 is tried (only when built with SONATA_USE_LIBGIT2).
 *   2. Validate it with Project::open(): no valid project, no install.
 *   3. Drop the .git directory and swap it into .sonata/deps/<name>.
 *   4. Repeat for whatever the new package depends on.
 *
 * A failed install never leaves a half-written package behind: the previous
 * version (if any) stays in place.
 *
 * The lock file lets "install" reproduce exactly what was installed before:
 * if deps/<name> is missing but the lock entry still matches the manifest,
 * the locked commit is fetched rather than whatever the branch points at now.
 * Only update() moves things forward.
 *
 * Not thread-safe; use one PackageManager per project from one thread.
 */

class PackageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/*
 * One way of getting a repository onto disk. PackageManager tries its
 * backends in order until one works.
 */
class GitBackend {
public:
    virtual ~GitBackend() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /* False if this backend can't be used at all (e.g. no git executable). */
    [[nodiscard]] virtual bool available() const = 0;

    /*
     * Clones "url" into "destination" (which must not exist yet) and checks
     * out "ref" (a branch, tag or commit; empty = the remote's default
     * branch). Returns the full hash of the commit that ended up checked out.
     * Throws PackageError on failure; the caller removes any partial result.
     */
    virtual std::string clone(
        const std::string& url,
        const std::filesystem::path& destination,
        const std::string& ref
    ) const = 0;
};

/* Shells out to the "git" executable found on PATH. */
[[nodiscard]] std::unique_ptr<GitBackend> makeCliGitBackend();

/* nullptr unless compiled with SONATA_USE_LIBGIT2 (and linked against libgit2). */
[[nodiscard]] std::unique_ptr<GitBackend> makeLibGit2Backend();

/* { CLI git, libgit2 (if compiled in) } */
[[nodiscard]] std::vector<std::unique_ptr<GitBackend>> defaultGitBackends();

struct InstalledPackage {
    Project::Dependency dependency; // url/ref may be empty if unknown (not in lock file)
    std::filesystem::path path;     // .sonata/deps/<name>
    std::string commit;             // exact commit; empty if unknown
    bool fresh = false;             // true if fetched by this call, false if already there
};

class PackageManager {
public:
    using Dependency = Project::Dependency;
    using ProgressCallback = std::function<void(const std::string&)>;

    /* "project" must outlive the manager. add() and remove() edit and save its manifest. */
    explicit PackageManager(Project& project);
    PackageManager(Project& project, std::vector<std::unique_ptr<GitBackend>> backends);

    PackageManager(const PackageManager&) = delete;
    PackageManager& operator=(const PackageManager&) = delete;

    /* Receives human-readable status lines ("fetching foo ..."). Optional. */
    void setProgressCallback(ProgressCallback callback);

    /*
     * Makes sure every dependency in the manifest (and theirs, recursively)
     * is present in .sonata/deps. Anything already installed and still
     * matching the manifest is left alone. Returns the whole resolved set in
     * install order.
     */
    std::vector<InstalledPackage> install();

    /* Re-fetches everything from its ref, ignoring the lock file. */
    std::vector<InstalledPackage> update();

    /* Re-fetches one package. Throws PackageError if it isn't a dependency. */
    std::vector<InstalledPackage> update(const std::string& name);

    /*
     * Adds (or replaces) a dependency in the manifest, saves the manifest and
     * installs it. If installing fails the manifest is put back as it was.
     */
    InstalledPackage add(Dependency dependency);

    /*
     * Removes a dependency from the manifest, saves it, then prunes. Returns
     * the names of all package folders that were deleted (the package itself
     * plus anything only it needed).
     */
    std::vector<std::string> remove(const std::string& name);

    /* Deletes every folder in .sonata/deps that nothing depends on any more. */
    std::vector<std::string> prune();

    /* Everything currently sitting in .sonata/deps, sorted by name. */
    [[nodiscard]] std::vector<InstalledPackage> installed() const;
    [[nodiscard]] bool isInstalled(const std::string& name) const;

    /*
     * Registers an "@name" alias on the loader for every installed package, so
     * require("@name/...") finds .sonata/deps/<name>/... Call before
     * ModuleLoader::install()/run(). (This is for "sn run"; "sn build" maps the
     * same folders into its virtual root instead.)
     */
    void registerAliases(luau::ModuleLoader& loader) const;

    [[nodiscard]] std::filesystem::path lockFile() const;

    /*
     * Checks a dependency declaration: name is a plain identifier (letters,
     * digits, '_'; doesn't start with a digit), url is non-empty and can't be
     * mistaken for a command-line option, and ref is a sane git ref.
     * Throws PackageError.
     */
    static void validate(const Dependency& dependency);

private:
    struct LockEntry {
        std::string url;
        std::string ref;
        std::string commit;
    };
    using Lock = std::map<std::string, LockEntry>;

    struct Refresh {
        bool all = false;
        std::set<std::string> names;
    };

    std::vector<InstalledPackage> sync(const Refresh& refresh);
    InstalledPackage ensureInstalled(const Dependency& dep, Lock& lock, const Refresh& refresh);
    InstalledPackage fetch(const Dependency& dep, const std::string& ref);
    std::string cloneWithBackends(
        const Dependency& dep,
        const std::filesystem::path& destination,
        const std::string& ref
    ) const;

    Lock readLock() const;
    void writeLock(const Lock& lock) const;
    void say(const std::string& message) const;

    Project& project_;
    std::vector<std::unique_ptr<GitBackend>> backends_;
    ProgressCallback progress_;
};

} // namespace sonata