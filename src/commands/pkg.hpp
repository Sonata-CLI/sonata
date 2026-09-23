#pragma once

#include "../cli/command.hpp"

namespace sonata::commands {

const cli::Command& pkg_command();

int pkg(int argc, char** argv);

}