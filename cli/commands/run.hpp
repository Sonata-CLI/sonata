#pragma once

#include "command.hpp"

namespace sonata::commands {

const cli::Command& run_command();

/*
 * sn run [path] [options] [-- <args>...]
 *
 * Runs a Sonata project through its entrypoint (or --entry), with the
 * project's dependencies available through require("@name").
 *
 * Returns 0 on success, 1 on any failure (bad arguments, no project, missing
 * dependencies, or the script raising an error).
 */
int run(int argc, char** argv);

}