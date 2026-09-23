#include "project.hpp"

#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sonata {

Project::Project(fs::path root)
    : root_(fs::absolute(std::move(root)).lexically_normal()),
      entrypoint_(root_ / "main.luau"),
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
        const fs::path entrypoint = current / "main.luau";
        const fs::path sonataDir = current / "sonata";

        if (fs::is_regular_file(entrypoint) &&
            fs::is_directory(sonataDir)) {
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

    if (!fs::exists(entrypoint_)) {
        throw std::runtime_error(
            "Project is missing main.luau: " + entrypoint_.string()
        );
    }

    if (!fs::is_regular_file(entrypoint_)) {
        throw std::runtime_error(
            "main.luau is not a regular file: " + entrypoint_.string()
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