#pragma once

#include "../cli/command.hpp"

namespace sonata::commands {

const cli::Command& init_command();

int init(int argc, char** argv);

}