#include "module.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lualib.h"

namespace fs = std::filesystem;

namespace sonata::luau {

namespace {

// Registry key of the table mapping module path -> module value
constexpr const char* kCacheKey = "sonata.modules";

constexpr const char* kSourceExtension = ".luau";
constexpr const char* kInitFile = "init.luau";

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

// "/a/b/c.luau" -> "/a/b". "/c.luau" -> "" (callers append '/' themselves)
std::string_view directoryOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// Keeps "loading_" accurate even when we leave a function early.
class LoadGuard {
public:
    LoadGuard(std::vector<std::string>& stack, std::string path)
        : stack_(stack) {
        stack_.push_back(std::move(path));
    }

    ~LoadGuard() {
        stack_.pop_back();
    }

    LoadGuard(const LoadGuard&) = delete;
    LoadGuard& operator=(const LoadGuard&) = delete;

private:
    std::vector<std::string>& stack_;
};

// Empty string if "path" isn't currently loading.
std::string describeCycle(const std::vector<std::string>& loading, const std::string& path) {
    auto it = std::find(loading.begin(), loading.end(), path);
    if (it == loading.end()) {
        return {};
    }

    std::string chain;
    for (; it != loading.end(); ++it) {
        chain += *it;
        chain += " -> ";
    }
    chain += path;

    return "cyclic require: " + chain;
}

// Pushes the cached value for "path" and returns true, or pushes nothing.
bool pushCached(lua_State* L, const std::string& path) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCacheKey);
    lua_getfield(L, -1, path.c_str());

    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return false;
    }

    lua_remove(L, -2); // drop the cache table, keep the value
    return true;
}

// Stores the value on top of the stack in the cache (leaves it there).
void cacheTop(lua_State* L, const std::string& path) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCacheKey);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, path.c_str());
    lua_pop(L, 1);
}

// The module path of the Lua code that called us, or "" if unknown.
//
// We walk past C frames so "pcall(require, "./x")" still resolves relative to
// the script that wrote the pcall. Only chunks we loaded ourselves count: their
// chunk name is "@<module path>".
std::string callerPath(lua_State* L) {
    lua_Debug ar;

    // Level 0 is `require` itself.
    for (int level = 1; lua_getinfo(L, level, "s", &ar); ++level) {
        if (std::strcmp(ar.what, "C") == 0) {
            continue;
        }

        if (ar.source != nullptr && ar.source[0] == '@') {
            return std::string(ar.source + 1);
        }

        break;
    }

    return {};
}

// Pushes "<caller file>:<line>: message" and returns -1 (see requireModule).
int failWithLocation(lua_State* L, const std::string& message) {
    luaL_where(L, 1);
    lua_pushlstring(L, message.data(), message.size());
    lua_concat(L, 2);
    return -1;
}

std::string errorMessage(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_tolstring(L, -1, &length);
    return std::string(text, length);
}

bool readModule(const ModuleSource& source, const std::string& path, Bytecode& out, std::string& error) {
    try {
        out = source.load(path);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    if (out.empty()) {
        error = "no bytecode was produced for module: " + path;
        return false;
    }

    return true;
}

// Loads "bytecode" into a fresh thread and runs it to completion (or first yield).
//
// The thread is left on top of L's stack, and its results (or the error value)
// are on the thread's own stack. Returns the lua_resume status; a load failure
// is reported as LUA_ERRSYNTAX with the message on top of the thread's stack.
int launch(lua_State* L, const std::string& path, const Bytecode& bytecode, lua_State*& thread) {
    // Create the thread from the *main* thread so it doesn't inherit the
    // globals of whoever called require (a sandboxed module thread, say),
    // otherwise modules would see each other's global writes.
    lua_State* mainThread = lua_mainthread(L);
    thread = lua_newthread(mainThread);
    lua_xmove(mainThread, L, 1); // anchor it on L's stack so it can't be collected

    luaL_sandboxthread(thread);

    const std::string chunkName = "@" + path;
    if (luau_load(thread, chunkName.c_str(), bytecode.luauData(), bytecode.size(), 0) != 0) {
        return LUA_ERRSYNTAX;
    }

    return lua_resume(thread, L, 0);
}

} // namespace


// Paths
std::optional<std::string> normalizePath(std::string_view input) {
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');

    std::string root;
    std::size_t pos = 0;

    if (path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':') {
        root = path.substr(0, 2) + "/";
        pos = 2;
    } else if (!path.empty() && path[0] == '/') {
        root = "/";
    } else {
        return std::nullopt; // not absolute
    }

    std::vector<std::string_view> parts;

    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string::npos) {
            next = path.size();
        }

        const std::string_view part(path.data() + pos, next - pos);

        if (part.empty() || part == ".") {
            // nothing
        } else if (part == "..") {
            if (parts.empty()) {
                return std::nullopt; // would climb above the root
            }
            parts.pop_back();
        } else {
            parts.push_back(part);
        }

        pos = next + 1;
    }

    std::string result = root;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            result += '/';
        }
        result += parts[i];
    }

    return result;
}


// Sources
bool FileSystemSource::exists(std::string_view path) const {
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::string(path)), ec);
}

Bytecode FileSystemSource::load(std::string_view path) const {
    // Note: on Windows, fs::path(std::string) assumes the ANSI code page. If you
    // need non-ASCII project paths there, convert from UTF-8 here.
    std::ifstream file(fs::path(std::string(path)), std::ios::binary);
    if (!file) {
        throw ModuleError("could not open module: " + std::string(path));
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    return compiler_.compile(contents.str());
}

void MemorySource::add(std::string_view path, Bytecode bytecode) {
    std::string rooted(path);
    if (rooted.empty() || (rooted[0] != '/' && rooted[0] != '\\')) {
        rooted.insert(0, "/");
    }

    auto normalized = normalizePath(rooted);
    if (!normalized) {
        throw std::invalid_argument("module path escapes the root: " + std::string(path));
    }

    files_[std::move(*normalized)] = std::move(bytecode);
}

bool MemorySource::exists(std::string_view path) const {
    return files_.find(std::string(path)) != files_.end();
}

Bytecode MemorySource::load(std::string_view path) const {
    auto it = files_.find(std::string(path));
    if (it == files_.end()) {
        throw ModuleError("module not found: " + std::string(path));
    }

    return it->second;
}


// ModuleLoader
ModuleLoader::ModuleLoader(std::unique_ptr<ModuleSource> source)
    : source_(std::move(source)) {
    if (!source_) {
        throw std::invalid_argument("ModuleLoader needs a ModuleSource");
    }
}

void ModuleLoader::addAlias(std::string name, std::string_view directory) {
    if (name.empty() || name.find('/') != std::string::npos) {
        throw std::invalid_argument("invalid alias name: '" + name + "'");
    }

    auto normalized = normalizePath(directory);
    if (!normalized) {
        throw std::invalid_argument(
            "alias '" + name + "' needs an absolute path, got: " + std::string(directory)
        );
    }

    aliases_[std::move(name)] = std::move(*normalized);
}

void ModuleLoader::install(VM& vm) {
    lua_State* L = vm.state();

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kCacheKey);

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, &ModuleLoader::requireCallback, "require", 1);
    lua_setglobal(L, "require");
}

void ModuleLoader::run(VM& vm, std::string_view entry) {
    const std::optional<std::string> path = normalizePath(entry);
    if (!path) {
        throw ModuleError("entry must be an absolute path: " + std::string(entry));
    }

    if (!source_->exists(*path)) {
        throw ModuleError("entry script not found: " + *path);
    }

    Bytecode bytecode;
    std::string error;
    if (!readModule(*source_, *path, bytecode, error)) {
        throw ModuleError(error);
    }

    lua_State* L = vm.state();

    // Registering the entry as "loading" means a dependency that requires it
    // back gets a proper cycle error instead of running it a second time.
    LoadGuard guard(loading_, *path);

    lua_State* thread = nullptr;
    const int status = launch(L, *path, bytecode, thread);

    if (status == LUA_OK) {
        lua_pop(L, 1); // the thread
        return;
    }

    // If you add a scheduler later, this is where yields stop being errors.
    std::string message = status == LUA_YIELD
        ? "thread yielded unexpectedly"
        : errorMessage(thread);

    message += "\nstacktrace:\n";
    message += lua_debugtrace(thread);

    lua_pop(L, 1); // the thread
    throw ModuleError(std::move(message));
}

// The function Lua actually calls. It deliberately holds no C++ objects with
// destructors: lua_error() may longjmp depending on how Luau was built, so all
// the real work happens in requireModule() and this just raises its result.
int ModuleLoader::requireCallback(lua_State* L) {
    auto* self = static_cast<ModuleLoader*>(lua_tolightuserdata(L, lua_upvalueindex(1)));

    std::size_t length = 0;
    const char* request = luaL_checklstring(L, 1, &length);

    const int results = self->requireModule(L, std::string_view(request, length));
    if (results < 0) {
        lua_error(L); // error value is on top of the stack
    }

    return results;
}

int ModuleLoader::requireModule(lua_State* L, std::string_view request) {
    std::string path;
    std::string error;

    if (!resolve(request, callerPath(L), path, error)) {
        return failWithLocation(L, error);
    }

    if (pushCached(L, path)) {
        return 1;
    }

    if (const std::string cycle = describeCycle(loading_, path); !cycle.empty()) {
        return failWithLocation(L, cycle);
    }

    Bytecode bytecode;
    if (!readModule(*source_, path, bytecode, error)) {
        return failWithLocation(L, error);
    }

    LoadGuard guard(loading_, path);

    lua_State* thread = nullptr;
    const int status = launch(L, path, bytecode, thread);

    if (status == LUA_YIELD) {
        return failWithLocation(L, "module yielded while loading (modules can't yield): " + path);
    }

    if (status != LUA_OK) {
        // Pass the module's own error along untouched. It may not be a string.
        lua_xmove(thread, L, 1);
        return -1;
    }

    if (lua_gettop(thread) != 1 || lua_isnil(thread, 1)) {
        return failWithLocation(L, "module must return exactly one non-nil value: " + path);
    }

    lua_xmove(thread, L, 1); // [thread, value]
    lua_remove(L, -2);       // [value]
    cacheTop(L, path);

    return 1;
}

bool ModuleLoader::resolve(
    std::string_view request,
    const std::string& caller,
    std::string& path,
    std::string& error
) const {
    const std::string shown(request);

    if (request.empty()) {
        error = "require path is empty";
        return false;
    }

    std::string target;

    if (request.front() == '@') {
        const std::size_t slash = request.find('/');
        const std::string name(request.substr(1, slash == std::string_view::npos ? slash : slash - 1));

        auto alias = aliases_.find(name);
        if (alias == aliases_.end()) {
            error = "unknown alias '@" + name + "' in require '" + shown + "'";
            return false;
        }

        target = alias->second;
        if (slash != std::string_view::npos) {
            target += '/';
            target += request.substr(slash + 1);
        }
    } else if (request == "." || request == ".." || startsWith(request, "./") || startsWith(request, "../")) {
        if (caller.empty()) {
            error = "can't resolve relative require '" + shown + "': the calling script isn't a known module";
            return false;
        }

        // Relative paths start at the directory of the requiring file. That
        // includes init.luau: "./helper" inside lib/init.luau means lib/helper.
        target = std::string(directoryOf(caller));
        target += '/';
        target += request;
    } else {
        error = "require path must start with './', '../' or '@' (got '" + shown + "')";
        return false;
    }

    const std::optional<std::string> normalized = normalizePath(target);
    if (!normalized) {
        error = "require '" + shown + "' climbs above the project root";
        return false;
    }

    // Candidates: <base>.luau and <base>/init.luau.
    const std::string& base = *normalized;
    const bool atRoot = base.back() == '/'; // "/" or "C:/" can't have a ".luau" sibling

    const std::string asFile = base + kSourceExtension;
    const std::string asInit = atRoot ? base + kInitFile : base + "/" + kInitFile;

    const bool fileExists = !atRoot && source_->exists(asFile);
    const bool initExists = source_->exists(asInit);

    if (fileExists && initExists) {
        error = "ambiguous require '" + shown + "': both " + asFile + " and " + asInit + " exist";
        return false;
    }

    if (!fileExists && !initExists) {
        error = "module not found: '" + shown + "' (looked for " +
                (atRoot ? asInit : asFile + " and " + asInit) + ")";
        return false;
    }

    path = fileExists ? asFile : asInit;
    return true;
}

} // namespace sonata::luau