#pragma warning (disable)

#include "test.hpp"
#include "../core/project.hpp"
#include "../core/ezruntime.hpp"
#include "../core/luau/vm.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace sonata::commands {

namespace {

// Exposed to Luau as sonata.log(...). Matches Global::Function's required
// signature: int (*)(VM&).
int debugLog(luau::VM& vm)
{
    lua_State* L = vm.state();
    const char* message = luaL_checkstring(L, 1);
    std::cout << "[luau] " << message << '\n';
    return 0;
}

// Everything after the project has been located.
int runModuleTest(Runtime& runtime, const fs::path& target)
{
    const Project& project = runtime.project();
    std::cout << "Project: " << project.manifest().name
              << " v" << project.manifest().version << '\n';

    for (const std::string& dep : runtime.missingDependencies()) {
        std::cerr << "warning: dependency '" << dep
                  << "' not found in " << project.dependenciesDir() << '\n';
    }

    runtime.environment().setFunction("log", &debugLog);

    // An explicit file argument tests that file; otherwise fall back to the
    // project's own entrypoint.
    const fs::path scriptPath = runtime.resolveScript(target);

    try {
        // Compiles scriptPath on its own first (a clear syntax error before
        // require() gets involved), then runs it through the full module
        // system, so require() and the project's environment behave
        // exactly as "sn run" would.
        const Runtime::RunResult result = runtime.debugRun(scriptPath);

        std::cout << "Compiled OK (" << result.bytecode.size() << " bytes, "
                  << result.compileMicros << " us)\n";
        std::cout << "Ran OK (" << result.runMicros << " us)\n";
    } catch (const luau::ModuleError& e) {
        std::cerr << "runtime error: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error in " << scriptPath << ": " << e.what() << '\n';
        return 1;
    }

    return 0;
}

} // namespace

int test(int argc, char** argv)
{
    // "sn test [path]" - path defaults to the current directory and
    // may point at either a project directory or a specific .luau file.
    const fs::path target = argc > 0 ? fs::path(argv[0]) : fs::current_path();

    try {
        Runtime runtime = Runtime::forPath(target);
        return runModuleTest(runtime, target);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

const cli::Command& test_command()
{
    static const cli::Command command{
        .name = "test",
        .description = "Debug purposes",
        .args = {sonata::cli::help_argument},
        .usage = {
            "sn test",
            "sn test <path>"
        },

        .examples = {
            "sn test",
            "sn test src/util.luau"
        },

        .execute = test,

        .children = {}

    };

    return command;
}

}