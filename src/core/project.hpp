#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sonata {

class Project {
public:
    /**
     * Open a Sonata project from a directory.
     *
     * The directory must contain:
     *   main.luau
     *   sonata/
     *
     * Throws std::runtime_error if the project is invalid.
     */
    static Project open(const std::filesystem::path& root);

    /**
     * Find and open a project starting from a path.
     *
     * If the path is a file, its parent directory is used.
     * The directory and its parents are searched for main.luau.
     */
    static Project find(const std::filesystem::path& start);

    const std::filesystem::path& root() const noexcept;
    const std::filesystem::path& entrypoint() const noexcept;
    const std::filesystem::path& sonataDir() const noexcept;
    const std::filesystem::path& projectFile() const noexcept;
    const std::filesystem::path& dependenciesDir() const noexcept;

    /**
     * Return all .luau source files in the project root.
     *
     * Files inside the sonata directory are excluded.
     */
    std::vector<std::filesystem::path> sourceFiles() const;

    bool hasDependenciesDirectory() const noexcept;

private:
    explicit Project(std::filesystem::path root);

    void validate();

    std::filesystem::path root_;
    std::filesystem::path entrypoint_;
    std::filesystem::path sonataDir_;
    std::filesystem::path projectFile_;
    std::filesystem::path dependenciesDir_;
};

} // namespace sonata