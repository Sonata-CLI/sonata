#include "project.hpp"

#include <algorithm>
#include <stdexcept>

#include "luau/datafile.hpp"

namespace fs = std::filesystem;

namespace sonata {

namespace {

std::string requireString(
    const luau::DataValue& table,
    const std::string& key,
    const fs::path& manifestPath
)
{
    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
    {
        throw std::runtime_error(
            manifestPath.string() + ": missing required field '" + key + "'"
        );
    }

    if (!field->isString())
    {
        throw std::runtime_error(
            manifestPath.string() + ": field '" + key + "' must be a string"
        );
    }

    return field->asString();
}

std::string optionalString(
    const luau::DataValue& table,
    const std::string& key,
    std::string fallback,
    const fs::path& manifestPath
)
{
    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
        return fallback;

    if (!field->isString())
    {
        throw std::runtime_error(
            manifestPath.string() + ": field '" + key + "' must be a string"
        );
    }

    return field->asString();
}

std::vector<std::string> optionalStringArray(
    const luau::DataValue& table,
    const std::string& key,
    const fs::path& manifestPath
)
{
    std::vector<std::string> result;

    const luau::DataValue* field = table.find(key);

    if (!field || field->isNil())
        return result;

    if (!field->isArray())
    {
        throw std::runtime_error(
            manifestPath.string() + ": field '" + key +
            "' must be a plain array of strings, e.g. { \"a\", \"b\" }"
        );
    }

    for (const auto& item : field->items())
    {
        if (!item.isString())
        {
            throw std::runtime_error(
                manifestPath.string() + ": every entry in '" + key +
                "' must be a string"
            );
        }

        result.push_back(item.asString());
    }

    return result;
}

} // namespace

Project::Project(fs::path root)
    : root_(fs::absolute(std::move(root)).lexically_normal()),
      sonataDir_(root_ / "sonata"),
      projectFile_(sonataDir_ / "project.luau"),
      dependenciesDir_(sonataDir_ / "deps") {
}

Project Project::open(const fs::path& root) {
    Project project(root);
    project.validate();

    return project;
}

Project Project::find(const fs::path& start) {
    fs::path current = fs::absolute(start).lexically_normal();

    // If the supplied path is a file, begin at its parent.
    if (fs::exists(current) && fs::is_regular_file(current)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        const fs::path sonataDir = current / "sonata";
        const fs::path projectFile = sonataDir / "project.luau";

        // The entrypoint filename is configurable (via project.luau), so it
        // can't be part of this detection heuristic; the manifest itself
        // is the one thing every Sonata project is guaranteed to have.
        if (fs::is_directory(sonataDir) &&
            fs::is_regular_file(projectFile)) {
            return Project::open(current);
        }

        const fs::path parent = current.parent_path();

        // We reached the filesystem root.
        if (parent == current) {
            break;
        }

        current = parent;
    }

    throw std::runtime_error(
        "Could not find a Sonata project from: " + start.string()
    );
}

void Project::validate() {
    if (!fs::exists(root_)) {
        throw std::runtime_error(
            "Project directory does not exist: " + root_.string()
        );
    }

    if (!fs::is_directory(root_)) {
        throw std::runtime_error(
            "Project path is not a directory: " + root_.string()
        );
    }

    if (!fs::exists(sonataDir_)) {
        throw std::runtime_error(
            "Project is missing the sonata directory: " + sonataDir_.string()
        );
    }

    if (!fs::is_directory(sonataDir_)) {
        throw std::runtime_error(
            "sonata is not a directory: " + sonataDir_.string()
        );
    }

    if (!fs::exists(projectFile_)) {
        throw std::runtime_error(
            "Project is missing project.luau: " + projectFile_.string()
        );
    }

    if (!fs::is_regular_file(projectFile_)) {
        throw std::runtime_error(
            "project.luau is not a regular file: " + projectFile_.string()
        );
    }

    if (fs::exists(dependenciesDir_) && !fs::is_directory(dependenciesDir_)) {
        throw std::runtime_error(
            "deps is not a directory: " + dependenciesDir_.string()
        );
    }

    // The sonata/deps/ folder is optional.
    //
    // project/
    // ├── main.luau
    // └── sonata/
    //
    // It will simply report false through the corresponding
    // hasDependenciesDirectory* function.

    loadManifest();
}

void Project::loadManifest() {
    luau::DataValue table;

    try {
        table = luau::DataFile::parseFile(projectFile_);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Failed to parse " + projectFile_.string() + ": " + e.what()
        );
    }

    if (!table.isTable()) {
        throw std::runtime_error(
            projectFile_.string() + " must return a table"
        );
    }

    manifest_.name = requireString(table, "name", projectFile_);
    manifest_.version = optionalString(table, "version", "0.0.0", projectFile_);
    manifest_.description = optionalString(table, "description", "", projectFile_);
    manifest_.license = optionalString(table, "license", "", projectFile_);
    manifest_.authors = optionalStringArray(table, "authors", projectFile_);
    manifest_.dependencies = optionalStringArray(table, "dependencies", projectFile_);
    manifest_.entrypoint = optionalString(table, "entrypoint", "main.luau", projectFile_);

    // Resolve + confine the entrypoint to the project root: project.luau
    // shouldn't be able to point outside of it (e.g. "../../etc/passwd").
    const fs::path candidate = (root_ / manifest_.entrypoint).lexically_normal();
    const fs::path relative = candidate.lexically_relative(root_);

    const bool escapesRoot = relative.empty() ||
        (relative.begin() != relative.end() && *relative.begin() == "..");

    if (escapesRoot) {
        throw std::runtime_error(
            projectFile_.string() + ": entrypoint '" + manifest_.entrypoint +
            "' resolves outside the project root"
        );
    }

    entrypoint_ = candidate;

    if (!fs::exists(entrypoint_)) {
        throw std::runtime_error(
            "Project is missing its entrypoint: " + entrypoint_.string()
        );
    }

    if (!fs::is_regular_file(entrypoint_)) {
        throw std::runtime_error(
            "entrypoint is not a regular file: " + entrypoint_.string()
        );
    }
}

const fs::path& Project::root() const noexcept {
    return root_;
}

const fs::path& Project::entrypoint() const noexcept {
    return entrypoint_;
}

const fs::path& Project::sonataDir() const noexcept {
    return sonataDir_;
}

const fs::path& Project::projectFile() const noexcept {
    return projectFile_;
}

const fs::path& Project::dependenciesDir() const noexcept {
    return dependenciesDir_;
}

const Project::Manifest& Project::manifest() const noexcept {
    return manifest_;
}

bool Project::hasDependenciesDirectory() const noexcept {
    return fs::is_directory(dependenciesDir_);
}

std::vector<fs::path> Project::sourceFiles() const {
    std::vector<fs::path> files;

    if (!fs::exists(root_)) {
        return files;
    }

    for (const auto& entry : fs::recursive_directory_iterator(root_)) {
        // Don't search inside sonata/.
        //
        // This is important because dependencies may themselves contain
        // Luau files, and those shouldn't automatically become part of
        // the project's source tree.
        if (entry.is_directory() && entry.path() == sonataDir_) {
            continue;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() != ".luau") {
            continue;
        }

        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());

    return files;
}

} // namespace sonata