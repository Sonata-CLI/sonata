#include "cli.hpp"
#include <iostream>
#include <string>
#include <vector>
#include "../commands/init.hpp"
#include "../commands/pkg.hpp"
#include "../commands/test.hpp"
namespace sonata::cli {

    namespace {

        const std::vector < Command > & commands() {
            static
            const std::vector < Command > commands = {
                commands::init_command(),
                commands::pkg_command(),
                commands::test_command()
            };

            return commands;
        }

        const Command * find_command(
            const std::vector < Command > & commands,
                const std::string & name
        ) {
            for (const auto & command: commands) {
                if (command.name == name)
                    return & command;
            }

            return nullptr;
        }

        void print_command_help(const Command & command) {
            std::cout << command.description << "\n\n";

            if (!command.usage.empty()) {
                std::cout << "Usage:\n";

                for (const auto & usage: command.usage)
                    std::cout << "  " << usage << '\n';

                std::cout << '\n';
            }

            if (!command.args.empty()) {
                std::cout << "Arguments:\n";

                for (const auto & argument: command.args) {
                    std::cout <<
                        "  " <<
                        argument.name;

                    if (!argument.aliases.empty()) {
                        std::cout << " (";

                        for (std::size_t i = 0; i < argument.aliases.size(); ++i) {
                            if (i > 0)
                                std::cout << ", ";

                            std::cout << argument.aliases[i];
                        }

                        std::cout << ")";
                    }

                    std::cout <<
                        "    " <<
                        argument.description <<
                        '\n';
                }

                std::cout << '\n';
            }

            if (!command.children.empty()) {
                std::cout << "Commands:\n";

                for (const auto & child: command.children) {
                    std::cout <<
                        "  " <<
                        child.name <<
                        "    " <<
                        child.description <<
                        '\n';
                }

                std::cout << '\n';
            }

            if (!command.examples.empty()) {
                std::cout << "Examples:\n";

                for (const auto & example: command.examples)
                    std::cout << "  " << example << '\n';

                std::cout << '\n';
            }
        }

        void print_help() {
            std::cout <<
                "Sonata - Luau runtime, compiler and package manager\n\n";

            std::cout << "Usage:\n";
            std::cout << "  sn <command> [arguments]\n\n";

            std::cout << "Commands:\n";

            for (const auto & command: commands()) {
                std::cout <<
                    "  " <<
                    command.name <<
                    "    " <<
                    command.description <<
                    '\n';
            }

            std::cout << "\nOptions:\n";
            std::cout << "  -h, --help       Show help\n";
            std::cout << "  -v, --version    Show version\n";
        }

    } // anonymous namespace

    int CLI::run(int argc, char ** argv) {
        if (argc < 2) {
            print_help();
            return 0;
        }

        const std::string first = argv[1];

        if (first == "--help" || first == "-h") {
            print_help();
            return 0;
        }

        if (first == "--version" || first == "-v") {
            std::cout << "sonata 0.1.0\n";
            return 0;
        }

        const Command * command =
            find_command(commands(), first);

        if (!command) {
            std::cerr <<
                "error: unknown command '" <<
                first <<
                "'\n\n";

            print_help();

            return 1;
        }

        if (argc >= 3) {
            const std::string second = argv[2];

            if (second == "--help" || second == "-h") {
                print_command_help( * command);
                return 0;
            }
        }

        if (!command -> execute) {
            print_command_help( * command);
            return 0;
        }

        return command -> execute(
            argc - 2,
            argv + 2
        );
    }

} // namespace sonata::cli