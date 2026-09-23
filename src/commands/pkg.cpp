#include "pkg.hpp"

#include <iostream>
#include <string>

namespace sonata::commands {
namespace {

const cli::Command& add_command()
{
    static const cli::Command command{
        .name = "add <packages>",
        .description = "Add (a) package(s) to the project (you can write more than one package)",

        .usage = {
            "sn pkg add <packages>"
        },

        .examples = {
            "sn pkg add fs",
            "sn pkg add fs http gui",
            "sn pkg add github:sonata/fs",
            "sn pkg add fs@1.3.0",
            "sn pkg add fs@unstable"
        }
    };

    return command;
}

const cli::Command& remove_command()
{
    static const cli::Command command{
        .name = "remove <packages>",
        .description = "Remove (a) package(s) from the project",

        .usage = {
            "sn pkg remove <packages>"
        },

        .examples = {
            "sn pkg remove fs http gui",
            "sn pkg remove fs"
        }
    };

    return command;
}

const cli::Command& list_command()
{
    static const cli::Command command{
        .name = "list",
        .description = "List installed packages",

        .usage = {
            "sn pkg list"
        },

        .examples = {
            "sn pkg list"
        }
    };

    return command;
}

const cli::Command& update_command()
{
    static const cli::Command command{
        .name = "update",
        .description = "Update project packages",

        .usage = {
            "sn pkg update"
        },

        .examples = {
            "sn pkg update"
        }
    };

    return command;
}

int add(int argc, char** argv)
{
    if (argc != 1)
    {
        std::cerr << "usage: sn pkg add <packages>\n";
        return 1;
    }

    std::cout << "Adding package: " << argv[0] << '\n';

    return 0;
}

int remove(int argc, char** argv)
{
    if (argc != 1)
    {
        std::cerr << "usage: sn pkg remove <packages>\n";
        return 1;
    }

    std::cout << "Removing package: " << argv[0] << '\n';

    return 0;
}

int list(int argc, char**)
{
    if (argc != 0)
    {
        std::cerr << "usage: sn pkg list\n";
        return 1;
    }

    std::cout << "No packages installed.\n";

    return 0;
}

int update(int argc, char**)
{
    if (argc != 0)
    {
        std::cerr << "usage: sn pkg update\n";
        return 1;
    }

    std::cout << "Updating packages...\n";

    return 0;
}

}

int pkg(int argc, char** argv)
{
    if (argc == 0)
    {
        std::cerr << "usage: sn pkg <command>\n";
        return 1;
    }

    const std::string command = argv[0];

    if (command == "add")
        return add(argc - 1, argv + 1);

    if (command == "remove")
        return remove(argc - 1, argv + 1);

    if (command == "list")
        return list(argc - 1, argv + 1);

    if (command == "update")
        return update(argc - 1, argv + 1);

    std::cerr << "error: unknown pkg command '" << command << "'\n";
    return 1;
}

const cli::Command& pkg_command()
{
    static const cli::Command command{
        .name = "pkg",
        .description = "Manage Sonata packages",
        .args = {
            {sonata::cli::help_argument}
        },
        .usage = {
            "sn pkg <command>"
        },

        .examples = {
            "sn pkg add fs",
            "sn pkg remove fs",
            "sn pkg update",
            "sn pkg list"
        },
        
        .execute = pkg,

        .children = {
            {
                .name = "add <packages>",
                .description = "Add (a) package(s) to the project",
                .args = {sonata::cli::help_argument},
                .execute = add
            },
            {
                .name = "remove <packages>",
                .description = "Remove (a) package(s) from the project",
                .args = {sonata::cli::help_argument},
                .execute = remove
            },
            {
                .name = "list",
                .description = "List installed packages",
                .args = {sonata::cli::help_argument},
                .execute = list
            },
            {
                .name = "update",
                .description = "Update project packages",
                .args = {sonata::cli::help_argument},
                .execute = update
            }
        }
    };

    return command;
}


}