#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sonata/core/luau/datafile.hpp>
#include <sonata/core/project.hpp>
#include "commands/init.hpp"

namespace fs = std::filesystem;

namespace sonata::commands {

int example(int argc, char **argv) {

    std::cout << "My example sonata command!";

    return 0;
}

const cli::Command &example_command() {
    static const cli::Argument example_arg{
        .name = "--example",
        .aliases = {"-ex"},
        .description =
            "Example argument"};
    static const cli::Command command{
        .name = "example",
        .description = "Example Sonata command",
        .args = {cli::help_argument, example_arg}, // help argument MUST be provided
        .usage = {"sn example"},                   // as its handled by cli.cpp

        .examples = {"sn example"},

        .execute = example,

        .children = {}

    };

    return command;
}

} // namespace sonata::commands