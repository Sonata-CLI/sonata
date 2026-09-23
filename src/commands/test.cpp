#pragma warning (disable)
#include "test.hpp"
#include "../core/project.hpp"
#include "../core/luau/vm.hpp"
#include "../core/luau/compiler.hpp"
#include "../core/luau/environment.hpp"

#include <optional>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>

namespace fs = std::filesystem;

std::string readFile(const fs::path& path)
{
    std::ifstream file(path);

    if (!file)
        throw std::runtime_error("Could not open file");

    std::stringstream buffer;
    buffer << file.rdbuf();

    return buffer.str();
}

std::string_view getView(const std::string& text) {
    return text;
}

namespace sonata::commands {

int test(int argc, char** argv)
{
    namespace fs = std::filesystem;
    using clock = std::chrono::steady_clock;

    std::string text = readFile("e.luau");
    std::string_view view = getView(text);

    luau::VM vm;
    luau::Compiler compiler;
    
    luau::Environment env;
    env.setString("greeting", "Hello, world!");
    bool loaded = env.load(vm, "greeting");

    if (!loaded)
    {
        std::cerr << "Failed to load greeting" << std::endl;
        return 1;
    }

    // Compile timing
    auto compileStart = clock::now();

    luau::Bytecode compiled = compiler.compile(view);

    auto compileEnd = clock::now();

    // Execution timing
    auto executeStart = clock::now();

    vm.execute(compiled);

    auto executeEnd = clock::now();

    const auto compileTime =
        std::chrono::duration<double, std::milli>(compileEnd - compileStart);

    const auto executeTime =
        std::chrono::duration<double, std::milli>(executeEnd - executeStart);

    std::cout << "Compile:  " << compileTime.count() << " ms\n";
    std::cout << "Execute:  " << executeTime.count() << " ms\n";

    return 0;
    
    
}

const cli::Command& test_command()
{
    static const cli::Command command{
        .name = "test",
        .description = "Debug purposes",
        .args = {sonata::cli::help_argument},
        .usage = {
            "sn test"
        },

        .examples = {
            "sn test"
        },
        
        .execute = test,
        
        .children = {}
        
    };

    return command;
}

}