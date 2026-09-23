#include "cli/cli.hpp"

int main(int argc, char** argv)
{
    sonata::cli::CLI cli;

    return cli.run(argc, argv);
}