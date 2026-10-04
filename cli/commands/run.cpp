#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lua.h"
#include "lualib.h"

#include <sonata/core/info.hpp>
#include <sonata/core/luau/environment.hpp>
#include <sonata/core/luau/vm.hpp>
#include <sonata/core/package.hpp>
#include <sonata/core/project.hpp>
#include <sonata/core/module.hpp>

#include "commands/run.hpp"

namespace fs = std::filesystem;

namespace sonata::commands {

/*
 * HOW "sn run" WORKS
 *
 *   1. Project         Project::find() locates .sonata/project.luau (which
 *                      reads it through DataFile) and the entrypoint.
 *   2. PackageManager  Makes sure the declared dependencies are installed
 *                      (--install fetches them, otherwise it's an error) and
 *                      registers an "@name" alias for each one.
 *   3. ModuleLoader    Serves require() from disk. FileSystemSource compiles
 *                      .luau files on demand with the Compiler.
 *   4. VM + Environment
 *                      Creates the VM, adds the "sonata" and "args" globals,
 *                      installs require(), sandboxes the globals.
 *   5. ModuleLoader::run() executes the entrypoint like any other module.
 */

namespace {

// argv[0] is the command itself ("run"); its arguments start after it.
constexpr int kFirstArgument = 1;

struct Options {
    fs::path start = ".";          // project directory, or any file inside it
    std::optional<fs::path> entry; // --entry, relative to the project root
    bool install = false;
    bool verbose = false;
    std::vector<std::string> scriptArgs; // everything after "--"
};

int fail(const std::string &message) {
    std::cerr << "error: " << message << '\n';
    return 1;
}

// Returns std::nullopt (after printing why) if the command line is invalid.
std::optional<Options> parseOptions(int argc, char **argv) {
    constexpr std::string_view entryPrefix = "--entry=";

    auto reject = [](const std::string &message) -> std::optional<Options> {
        fail(message);
        return std::nullopt;
    };

    Options options;
    bool haveStart = false;

    for (int i = kFirstArgument; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--") {
            for (++i; i < argc; ++i) {
                options.scriptArgs.emplace_back(argv[i]);
            }
            break;
        }

        if (arg == "--install" || arg == "-i") {
            options.install = true;
        } else if (arg == "--verbose" || arg == "-v") {
            options.verbose = true;
        } else if (arg == "--entry" || arg == "-e") {
            if (i + 1 >= argc) {
                return reject("--entry needs a file");
            }
            options.entry = fs::path(argv[++i]);
        } else if (arg.starts_with(entryPrefix)) {
            options.entry = fs::path(std::string(arg.substr(entryPrefix.size())));
        } else if (arg.size() > 1 && arg.front() == '-') {
            return reject("unknown option '" + std::string(arg) +
                          "' (use -- to pass options to the script)");
        } else if (haveStart) {
            return reject("unexpected argument '" + std::string(arg) +
                          "' (use -- to pass arguments to the script)");
        } else {
            options.start = fs::path(std::string(arg));
            haveStart = true;
        }
    }

    if (options.entry && options.entry->empty()) {
        return reject("--entry needs a file");
    }

    return options;
}

/*
 * The module path ModuleLoader::run() wants: absolute, forward slashes,
 * normalised. Defaults to the manifest's entrypoint.
 */
std::string resolveEntry(const Project &project,
                         const std::optional<fs::path> &entryOverride) {
    fs::path file = project.entrypoint();

    if (entryOverride) {
        file = project.root() / *entryOverride;
        if (!fs::is_regular_file(file)) {
            throw std::runtime_error("entry file not found: " +
                                     file.generic_string());
        }
    }

    auto path = luau::normalizePath(fs::absolute(file).generic_string());
    if (!path) {
        throw std::runtime_error("can't use '" + file.generic_string() +
                                 "' as a module path");
    }
    return *path;
}

/*
 * With --install, installs whatever is missing (install() leaves anything that
 * is already present and still matching the manifest alone). Without it,
 * running with missing dependencies is an error rather than a surprise
 * download.
 */
void prepareDependencies(PackageManager &packages, const Project &project,
                         bool install) {
    if (install) {
        packages.setProgressCallback(
            [](const std::string &line) { std::cerr << line << '\n'; });
        packages.install();
        return;
    }

    std::string missing;
    for (const auto &dependency : project.manifest().dependencies) {
        if (packages.isInstalled(dependency.name)) {
            continue;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += dependency.name;
    }

    if (!missing.empty()) {
        throw PackageError("missing dependencies: " + missing +
                           " (pass --install to fetch them)");
    }
}

/*
 * The "sonata" global:
 *
 *     sonata.version          "0.1.0"
 *     sonata.platform         "linux-x64"
 *     sonata.entry            module path of the script being run
 *     sonata.project.name     from the manifest
 *     sonata.project.version  from the manifest
 *     sonata.project.root     absolute path of the project folder
 */
luau::EnvNamespace makeSonataNamespace(const Project &project,
                                       const std::string &entry) {
    luau::EnvNamespace projectInfo;
    projectInfo.setString("name", project.manifest().name);
    projectInfo.setString("version", project.manifest().version);
    projectInfo.setString("root", project.root().generic_string());

    luau::EnvNamespace sonata;
    sonata.setString("version", info::version_string());
    sonata.setString("platform", info::platform_string());
    sonata.setString("entry", entry);
    sonata.set("project",
               std::make_shared<luau::EnvNamespace>(std::move(projectInfo)));
    return sonata;
}

/*
 * The "args" global: an array of the strings given after "--" (empty if
 * none). Set before luaL_sandbox(), which makes it read-only.
 *
 * It's built with the Lua API rather than through Environment because
 * Environment keys everything by name, so it can't express a real array.
 */
void setScriptArguments(lua_State *L, const std::vector<std::string> &args) {
    lua_createtable(L, static_cast<int>(args.size()), 0);
    for (std::size_t i = 0; i < args.size(); ++i) {
        lua_pushlstring(L, args[i].data(), args[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, LUA_GLOBALSINDEX, "args");
}

void printSummary(const Project &project, const std::string &entry,
                  const PackageManager *packages) {
    const Project::Manifest &manifest = project.manifest();

    std::string names;
    if (packages != nullptr) {
        for (const auto &package : packages->installed()) {
            if (!names.empty()) {
                names += ", ";
            }
            names += package.dependency.name;
        }
    }

    std::cerr << "project   " << manifest.name << ' ' << manifest.version
              << " (" << project.root().generic_string() << ")\n"
              << "entry     " << entry << '\n'
              << "packages  " << (names.empty() ? "none" : names) << '\n';
}

} // namespace

int run(int argc, char **argv) {
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        return 1;
    }

    try {
        // 1. The project.
        if (!fs::exists(options->start)) {
            return fail("path does not exist: " + options->start.string());
        }

        Project project = Project::find(fs::canonical(options->start));
        const std::string entry = resolveEntry(project, options->entry);

        // 2. Dependencies. Skipped entirely for projects that have none.
        std::optional<PackageManager> packages;
        if (options->install || !project.manifest().dependencies.empty() ||
            project.hasDependenciesDirectory()) {
            packages.emplace(project);
            prepareDependencies(*packages, project, options->install);
        }

        if (options->verbose) {
            printSummary(project, entry, packages ? &*packages : nullptr);
        }

        // 3. The module system. The loader must outlive the VM (it's
        //    declared first, so it's destroyed last) and must not move.
        luau::ModuleLoader loader(std::make_unique<luau::FileSystemSource>());
        if (packages) {
            packages->registerAliases(loader);
        }

        // 4. The VM and its globals.
        luau::VM vm;

        luau::Environment environment;
        environment.setNamespace("sonata", makeSonataNamespace(project, entry));
        environment.load(vm);
        setScriptArguments(vm.state(), options->scriptArgs);

        // require() has to be defined before the globals are made read-only.
        loader.install(vm);
        luaL_sandbox(vm.state());

        // 5. Go. Throws ModuleError (with a stack trace) if the script fails.
        loader.run(vm, entry);
        return 0;
    } catch (const luau::ModuleError &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        return fail(error.what());
    }
}

const cli::Command &run_command() {
    static const cli::Argument entry_arg{
        .name = "--entry",
        .aliases = {"-e"},
        .description = "Run <file> (relative to the project root) instead of "
                       "the manifest's entrypoint"};
    static const cli::Argument verbose_arg{
        .name = "--verbose",
        .aliases = {"-v"},
        .description = "Print the project, entry and packages being used"};

    static const cli::Command command{
        .name = "run",
        .description = "Run a project without compiling it (aka live test)",
        .args = {cli::help_argument, entry_arg, verbose_arg},
        .usage = {"sn run [path] [options] [-- <args>...]"},
        .examples = {"sn run",
                     "sn run ./my-project",
                     "sn run --entry tools/seed.luau",
                     "sn run -- --port 8080"},

        .execute = run,

        .children = {}

    };

    return command;
}

} // namespace sonata::commands