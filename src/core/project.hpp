#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sonata {

class Project {
public:
    /*
     * Parsed contents of sonata/project.luau.
     *
     * "entrypoint" is stored exactly as written in the manifest (e.g.
     * "main.luau" or "src/init.luau"); Project::entrypoint() gives you the
     * resolved, absolute path.
     */
    struct Manifest {
        std::string name;
        std::string version;
        std::string description;
        std::vector<std::string> authors;
        std::string license;
        std::string entrypoint;
        std::vector<std::string> dependencies;
    };

    /**
     * Open a Sonata project from a directory.
     *
     * The directory must contain:
     *   sonata/project.luau
     *   <entrypoint>            (as named by project.luau; "main.luau" if
     *                            the manifest doesn't specify one)
     *
     * Throws std::runtime_error if the project is invalid, or if
     * project.luau can't be parsed.
     */
    static Project open(const std::filesystem::path& root);

    /**
     * Find and open a project starting from a path.
     *
     * If the path is a file, its parent directory is used.
     * The directory and its parents are searched for sonata/project.luau.
     */
    static Project find(const std::filesystem::path& start);

    const std::filesystem::path& root() const noexcept;
    const std::filesystem::path& entrypoint() const noexcept;
    const std::filesystem::path& sonataDir() const noexcept;
    const std::filesystem::path& projectFile() const noexcept;
    const std::filesystem::path& dependenciesDir() const noexcept;

    const Manifest& manifest() const noexcept;

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
    void loadManifest();

    std::filesystem::path root_;
    std::filesystem::path sonataDir_;
    std::filesystem::path projectFile_;
    std::filesystem::path dependenciesDir_;
    std::filesystem::path entrypoint_;

    Manifest manifest_;
};

} // namespace sonata