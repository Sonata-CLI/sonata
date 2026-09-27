#include "ezruntime.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sonata
{

    Runtime::Runtime(Project project, std::unique_ptr<luau::ModuleSource> source)
        : project_(std::move(project)),
            loader_(std::make_unique<luau::ModuleLoader>(
                source ? std::move(source) : std::make_unique<luau::FileSystemSource>())),
            vm_(std::make_unique<luau::VM>())
    {
        if (project_.hasDependenciesDirectory())
        {
            for (const std::string &dependency : project_.manifest().dependencies)
            {
                const fs::path depDir = project_.dependenciesDir() / dependency;

                if (fs::is_directory(depDir))
                {
                    loader_->addAlias(dependency, depDir.string());
                }
                else
                {
                    missingDependencies_.push_back(dependency);
                }
            }
        }

        loader_->install(*vm_);
    }

    Runtime Runtime::forPath(
        const fs::path &start,
        std::unique_ptr<luau::ModuleSource> source)
    {
        return Runtime(Project::find(start), std::move(source));
    }

    const Project &Runtime::project() const noexcept
    {
        return project_;
    }

    luau::VM &Runtime::vm() noexcept
    {
        return *vm_;
    }

    luau::Environment &Runtime::environment() noexcept
    {
        return environment_;
    }

    luau::ModuleLoader &Runtime::loader() noexcept
    {
        return *loader_;
    }

    const std::vector<std::string> &Runtime::missingDependencies() const noexcept
    {
        return missingDependencies_;
    }

    void Runtime::reload()
    {
        environment_.load(*vm_);
    }

    luau::Bytecode Runtime::compile(std::string_view source) const
    {
        luau::Compiler compiler;
        return compiler.compile(source);
    }

    void Runtime::run(std::string_view modulePath)
    {
        reload();
        loader_->run(*vm_, modulePath);
    }

    void Runtime::runEntrypoint()
    {
        const std::optional<std::string> entry =
            luau::normalizePath(project_.entrypoint().string());

        if (!entry)
        {
            throw luau::ModuleError(
                "could not normalize entrypoint path: " + project_.entrypoint().string());
        }

        run(*entry);
    }

    fs::path Runtime::resolveScript(const fs::path &target) const
    {
        if (fs::is_regular_file(target))
        {
            return fs::absolute(target).lexically_normal();
        }

        return project_.entrypoint();
    }

    std::string Runtime::readSource(const fs::path &path)
    {
        std::ifstream file(path);

        if (!file)
        {
            throw std::runtime_error("could not open file: " + path.string());
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();

        return buffer.str();
    }

    Runtime::RunResult Runtime::debugRun(const fs::path &scriptPath)
    {
        const std::string source = readSource(scriptPath);

        const auto compileStart = std::chrono::steady_clock::now();
        luau::Bytecode bytecode = compile(source);
        const auto compileUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - compileStart)
            .count();

        const std::optional<std::string> entry =
            luau::normalizePath(scriptPath.string());

        if (!entry)
        {
            throw luau::ModuleError("could not normalize path: " + scriptPath.string());
        }

        const auto runStart = std::chrono::steady_clock::now();
        run(*entry);
        const auto runUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - runStart)
            .count();

        return RunResult{std::move(bytecode), compileUs, runUs};
    }

} // namespace sonata