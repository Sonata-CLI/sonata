#include "init.hpp"
#include "../core/project.hpp"

#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

namespace fs = std::filesystem;

/*  Argument Parsing:
    Parses a single positional path argument.
    Returns std::nullopt if no path was supplied or a path was already specified.
    Throws no exceptions.    
*/
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

/*  Validation:
    Checks wether a path can be safely treated as a directory path.
    This checks every existing component of the path. If an existing
    component is a regular file, the path is invalid.

    Non-existent components are also allowed.
*/
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

namespace sonata::commands {

int init(int argc, char** argv) {
    namespace fs = std::filesystem;

    fs::path specifiedpath;
    bool pathspecified = false;
    bool validate = false;

    std::vector<std::string> args(argv, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
    
        if (arg.empty())
            continue;
    
        // Options
        if (arg[0] == '-') {
            if (arg == "--validate") {
                validate = true;
            } else {
                std::cerr << "error: unknown argument " << arg << "\n";
                return 1;
            }
        }
    
        // Path
        else {
            auto path = parse_path_arg(arg, pathspecified);

            if (!path) {
                std::cerr << "error: unexpected argument " << arg << "\n";
                return 1;
            }

            specifiedpath = *path;
        }
    }
    fs::path root = pathspecified
        ? specifiedpath
        : fs::current_path();
    
    std::string patherror;
    
    if (!validate_directory_path(root, patherror)) {
        std::cerr << "error: " << patherror << "\n";
        return 1;
    }
    
    std::error_code ec;
    
    if (!fs::exists(root, ec)) {
        if (!fs::create_directories(root, ec)) {
            std::cerr << "error: could not create directory '"
                    << root << "': "
                    << ec.message() << "\n";
            return 1;
        }
    }

    fs::path sonata = root / "sonata";
    
    if (validate) {
        std::optional<sonata::Project> project;
        try {
            project = sonata::Project::open(".");
        } catch (const std::runtime_error& e) {
            std::cerr << "error: " << e.what() << "\n";
            std::cout << "possible fix: run 'sn init' (run 'sn init -h' to see info)\n";
            return 1;
        }
        if (!project) {
            std::cerr << "error: project couldn't be declared/opened due to an unknown error\n";
            return 1;
        }
        std::cout << "success\n";
        std::cout << "project root: " << project->root() << "\n";
        std::cout << "sonataDir: " << project->sonataDir() << "\n";
        std::cout << "entry point: " << project->entrypoint() << "\n";
        std::cout << "has dependency folder: " << std::boolalpha << project->hasDependenciesDirectory() << "\n";
        return 0;
    }
    
    try {
        fs::create_directories(sonata / "deps");

        std::ofstream project(sonata / "project.luau");
        std::ofstream mainluau(root / "main.luau");

        project << "return {}\n";
        mainluau << "print('Hello World!')";
        std::cout << "Initialized Sonata project at " << root << "\n";
    } catch (const fs::filesystem_error& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

const cli::Command& init_command()
{
    static const cli::Argument validate_arg{
        .name = "--validate",
        .aliases = {},
        .description = "Prints if the specified directory is a valid Sonata project"
    };
    static const cli::Command command{
        .name = "init",
        .description = "Create a new Sonata project",
        .args = {cli::help_argument, validate_arg},
        .usage = {
            "sn init"
        },

        .examples = {
            "sn init"
        },
        
        .execute = init,
        
        .children = {}
        
    };

    return command;
}

}