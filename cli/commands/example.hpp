#pragma once

#include "command.hpp"

namespace sonata::commands {

const cli::Command& example_command();

int example(int argc, char** argv);

}