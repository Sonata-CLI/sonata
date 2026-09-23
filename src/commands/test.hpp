#pragma once

#include "../cli/command.hpp"

namespace sonata::commands {

int test(int argc, char** argv);

const cli::Command& test_command();

}