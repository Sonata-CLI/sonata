#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sonata::cli {

struct Argument
{
    std::string name;
    std::vector<std::string> aliases;
    std::string description;
};

struct Command
{
    std::string name;
    std::string description;
    std::vector<Argument> args;
    std::vector<std::string> usage;
    std::vector<std::string> examples;
    std::function<int(int argc, char** argv)> execute;
    std::vector<Command> children;
};

inline const Argument help_argument{
    .name = "--help",
    .aliases = {"-h"},
    .description = "Shows help information about the command or subcommand"
};

}