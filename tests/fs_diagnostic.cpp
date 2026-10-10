// Temporary diagnostic: replaces the single big fs test.
// One Exec per assertion group, so ONE CI run lists every group that fails
// ("FAILED GROUP: ..."). Each group starts from the same fixture, cleans up
// after itself, and re-raises the original Lua error afterwards.
//
// Needs <string> in addition to whatever test_builtins.cpp already includes.

#include <string>
#include "doctest.h"
#include "helper.hpp"

TEST_CASE("fs diagnostic: one Exec per assertion group") {
    // Same start as the original test: temp dir, source/nested/data.bin = "A\0BC".
    const std::string fixture = R"lua(
        local fs = require("@sonata/fs")
        local root = fs.realPath(fs.makeTempDir("sonata-test-"))
        local source = root .. "/source"
        fs.makeDir(source .. "/nested", true)
        local file = source .. "/nested/data.bin"
        fs.writeFile(file, "A\0B")
        fs.appendFile(file, "C")
    )lua";

    struct Group {
        const char* name;
        const char* body;
        bool informational; // a failure here is only a warning
    };

    const Group groups[] = {
        {"constants", R"lua(
            assert(fs.version == 1 and fs.maxReadSize > 0)
            assert(fs.modes.file == 420 and fs.modes.privateDirectory == 448)
        )lua", false},

        {"readFile", R"lua(
            assert(fs.readFile(file) == "A\0BC")
        )lua", false},

        {"exists / isFile / isDir", R"lua(
            assert(fs.exists(file))
            assert(fs.isFile(file))
            assert(not fs.isDir(file))
        )lua", false},

        {"stat", R"lua(
            assert(fs.stat(file).kind == "file")
            assert(fs.stat(file).size == 4)
        )lua", false},

        {"lstat", R"lua(
            assert(fs.lstat(file).kind == "file")
        )lua", false},

        {"realPath(file) == file", R"lua(
            assert(fs.realPath(file) == file)
        )lua", false},

        {"copyFile", R"lua(
            local copied = root .. "/copied.bin"
            fs.copyFile(file, copied)
            assert(fs.readFile(copied) == "A\0BC")
        )lua", false},

        {"move", R"lua(
            local copied = root .. "/copied.bin"
            local moved = root .. "/moved.bin"
            fs.copyFile(file, copied)
            fs.move(copied, moved)
            assert(not fs.exists(copied))
            assert(fs.readFile(moved) == "A\0BC")
        )lua", false},

        {"listDir recursive", R"lua(
            local names = fs.listDir(source, true)
            assert(#names == 2)
            assert(names[1] == "nested")
            assert(names[2] == "nested/data.bin")
        )lua", false},

        {"readDir recursive", R"lua(
            local entries = fs.readDir(source, true)
            assert(#entries == 2)
            assert(entries[2].name == "nested/data.bin")
            assert(entries[2].kind == "file")
        )lua", false},

        {"copyDir", R"lua(
            fs.copyDir(source, root .. "/destination")
            assert(fs.readFile(root .. "/destination/nested/data.bin") == "A\0BC")
        )lua", false},

        {"chmod readOnly round-trip", R"lua(
            fs.chmod(file, fs.modes.readOnly)
            assert(fs.stat(file).mode % 512 == fs.modes.readOnly)
        )lua", false},

        {"chmod file round-trip", R"lua(
            fs.chmod(file, fs.modes.readOnly)
            fs.chmod(file, fs.modes.file)
            assert(fs.stat(file).mode % 512 == fs.modes.file)
        )lua", false},

        // The ORIGINAL assertion. Expected to fail on Windows by design.
        {"chmod private round-trip (Windows cannot do this)", R"lua(
            fs.chmod(file, fs.modes.private)
            assert(fs.stat(file).mode % 512 == fs.modes.private)
        )lua", true},

        // The symlink groups silently pass when the OS refuses to create the link.
        {"symlink: isSymlink", R"lua(
            local target = root .. "/target.bin"
            fs.copyFile(file, target)
            local link = root .. "/link"
            if not pcall(fs.symlink, target, link) then return end
            assert(fs.isSymlink(link))
        )lua", false},

        {"symlink: readLink == target", R"lua(
            local target = root .. "/target.bin"
            fs.copyFile(file, target)
            local link = root .. "/link"
            if not pcall(fs.symlink, target, link) then return end
            assert(fs.readLink(link) == target)
        )lua", false},

        {"symlink: stat / lstat kinds", R"lua(
            local target = root .. "/target.bin"
            fs.copyFile(file, target)
            local link = root .. "/link"
            if not pcall(fs.symlink, target, link) then return end
            assert(fs.stat(link).kind == "file")
            assert(fs.lstat(link).kind == "symlink")
        )lua", false},

        {"tempDir / missing file", R"lua(
            assert(fs.tempDir() ~= "")
            assert(not fs.isFile(root .. "/missing"))
        )lua", false},

        {"cwd / chdir", R"lua(
            local originalCwd = fs.cwd()
            fs.chdir(root)
            local ok, err = pcall(function() assert(fs.cwd() == root) end)
            fs.chdir(originalCwd)
            assert(ok, err)
        )lua", false},

        {"removeFile", R"lua(
            fs.removeFile(file)
            assert(not fs.exists(file))
        )lua", false},

        {"removeFile on a read-only file", R"lua(
            fs.chmod(file, fs.modes.readOnly)
            fs.removeFile(file)
            assert(not fs.exists(file))
        )lua", false},

        {"removeDir recursive", R"lua(
            fs.removeDir(root, true)
            assert(not fs.exists(root))
        )lua", false},

        {"removeDir recursive with a read-only file inside", R"lua(
            fs.chmod(file, fs.modes.readOnly)
            fs.removeDir(root, true)
            assert(not fs.exists(root))
        )lua", false},

        {"removeDir recursive with a symlink inside", R"lua(
            local target = root .. "/target.bin"
            fs.copyFile(file, target)
            if not pcall(fs.symlink, target, root .. "/link") then return end
            fs.removeDir(root, true)
            assert(not fs.exists(root))
        )lua", false},

        // Prints what the library returns. Only visible if Exec forwards
        // stdout to the CI log; never fails the test.
        {"value dump (stdout)", R"lua(
            local target = root .. "/target.bin"
            fs.copyFile(file, target)
            local link = root .. "/link"
            local linked = pcall(fs.symlink, target, link)
            pcall(print, "tempDir        =", fs.tempDir())
            pcall(print, "root           =", root)
            pcall(print, "realPath(file) =", fs.realPath(file))
            pcall(print, "file           =", file)
            pcall(print, "mode(file)     =", fs.stat(file).mode)
            pcall(print, "symlink made   =", linked)
            if linked then
                pcall(print, "readLink       =", fs.readLink(link))
                pcall(print, "target         =", target)
            end
        )lua", true},
    };

    for (const Group& group : groups) {
        const std::string script = fixture +
            "local ok, err = pcall(function()\n" + group.body + "\nend)\n"
            "pcall(fs.removeDir, root, true)\n"      // cleanup, never fails the group
            "if not ok then error(err, 0) end\n";

        Exec exec(script.c_str());
        if (group.informational) {
            WARN_MESSAGE(exec.getStatus() == 0, "group differs: " << group.name);
        } else {
            CHECK_MESSAGE(exec.getStatus() == 0, "FAILED GROUP: " << group.name);
        }
    }
}