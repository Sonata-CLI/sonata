// @sonata/process: facts about the running process, and ways to act on it.
//
//     local process = require("@sonata/process")
//
//     process.os            --> "windows" | "macos" | "linux" | "freebsd" | "android" | "unknown"
//     process.arch          --> "x86_64" | "aarch64" | "x86" | "arm" | "riscv64" | "unknown"
//     process.endianness    --> "little" | "big"
//     process.pid           --> 4242
//     process.execPath      --> "/usr/local/bin/sonata"        (nil if it cannot be determined)
//     process.args          --> { "build", "--release" }        (set by the host via process::setArguments)
//
//     process.cwd()                  --> "/home/me/project"
//     process.chdir("/tmp")
//     process.exit(code)             -- never returns; code defaults to 0
//
//     process.env.HOME               --> "/home/me"             (nil if unset)
//     process.env.FOO = "bar"        -- assigning nil removes the variable
//     for name, value in process.env do ... end
//
//     local result = process.exec("git", { "status", "--short" }, {
//         cwd = "repo",                -- working directory of the child
//         env = { GIT_PAGER = "cat" }, -- added to / overriding the inherited environment
//         stdin = "text",              -- data fed to the child's stdin
//         shell = false,               -- true: run "program args..." through /bin/sh (cmd.exe on Windows)
//         stdio = "pipe",              -- "pipe" captures stdout/stderr, "inherit" shares ours
//     })
//     result.ok, result.code, result.stdout, result.stderr
//
// Notes:
//   * process.exec blocks the whole VM until the child exits, so other tasks do not run meanwhile.
//     A program that cannot be started raises an error; one that exits non-zero does not (ok = false).
//   * A child killed by a signal reports code 128 + signal, like a shell would.
//   * With stdio = "inherit" the child shares the terminal and result.stdout / result.stderr are "".
//   * process.exit uses std::exit: stdio is flushed and atexit handlers run, but stack objects of
//     the host are not unwound. Register cleanup with std::atexit if it must always happen.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

// host API ///////////////
//
// Host-side configuration for @sonata/process.
namespace sonata::lib::libs::process {

// Sets what scripts see as "process.args", e.g. the command-line arguments that
// follow the script path. Call it before the script first requires
// "@sonata/process": the library reads the list once, when it is opened. If it
// is never called, "process.args" is an empty table.
void setArguments(lua_State* L, const std::vector<std::string>& args);

} // namespace sonata::lib::libs::process

#if !defined(_WIN32) && !defined(__APPLE__)
extern char** environ;
#endif

namespace sonata::lib::libs {

namespace {

constexpr const char* kArgsRegistryKey = "sonata.process.args";

// platform layer /////////////
//
// Plain C++: nothing in here touches Lua, so nothing in here can raise a Lua
// error. The bindings below call it, then turn failures into Lua errors only
// after every C++ object is out of scope (see the rule at the top of example.cpp).

namespace sys {

using EnvList = std::vector<std::pair<std::string, std::string>>;

std::string errorText(int code) {
    return std::system_category().message(code);
}

const char* osName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

const char* archName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unknown";
#endif
}

const char* endiannessName() {
    const std::uint16_t probe = 1;
    unsigned char first = 0;
    std::memcpy(&first, &probe, 1);
    return first == 1 ? "little" : "big";
}

#if defined(_WIN32)

// Strings cross the Lua boundary as UTF-8; Windows is spoken to in UTF-16.
std::wstring widen(const char* text, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), wide.data(), count);
    }
    return wide;
}

std::wstring widen(const std::string& text) {
    return widen(text.data(), text.size());
}

std::string narrow(const wchar_t* text, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(count), '\0');
    if (count > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), utf8.data(), count, nullptr, nullptr);
    }
    return utf8;
}

using WideEnvList = std::vector<std::pair<std::wstring, std::wstring>>;

WideEnvList currentEnvironmentWide() {
    WideEnvList vars;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) {
        return vars;
    }

    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const wchar_t* equals = std::wcschr(entry + 1, L'='); // +1: "=C:=C:\" style entries start with '='
        if (!equals || entry[0] == L'=') {
            continue;
        }
        vars.emplace_back(std::wstring(entry, equals), std::wstring(equals + 1));
    }

    FreeEnvironmentStringsW(block);
    return vars;
}

bool sameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool getEnv(const char* name, std::string& value) {
    const std::wstring wideName = widen(name, std::strlen(name));
    std::wstring buffer(64, L'\0');

    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetEnvironmentVariableW(wideName.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));

        if (n == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return false;
            }
            value.clear(); // exists, but empty
            return true;
        }
        if (n < buffer.size()) {
            value = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(n); // too small: n is the required size including the terminator
    }
}

// value == nullptr removes the variable.
bool setEnv(const char* name, const char* value, std::string& error) {
    const std::wstring wideName = widen(name, std::strlen(name));
    const std::wstring wideValue = value ? widen(value, std::strlen(value)) : std::wstring();

    if (SetEnvironmentVariableW(wideName.c_str(), value ? wideValue.c_str() : nullptr)) {
        return true;
    }
    if (!value && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return true; // removing something that is not there is fine
    }
    error = errorText(static_cast<int>(GetLastError()));
    return false;
}

void listEnv(EnvList& out) {
    for (const auto& [name, value] : currentEnvironmentWide()) {
        out.emplace_back(narrow(name.c_str(), name.size()), narrow(value.c_str(), value.size()));
    }
}

bool currentDir(std::string& path, std::string& error) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (n == 0) {
            error = errorText(static_cast<int>(GetLastError()));
            return false;
        }
        if (n < buffer.size()) {
            path = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(n);
    }
}

bool changeDir(const char* path, std::string& error) {
    const std::wstring widePath = widen(path, std::strlen(path));
    if (SetCurrentDirectoryW(widePath.c_str())) {
        return true;
    }
    error = errorText(static_cast<int>(GetLastError()));
    return false;
}

bool executablePath(std::string& path) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return false;
        }
        if (n < buffer.size()) {
            path = narrow(buffer.data(), n);
            return true;
        }
        buffer.resize(buffer.size() * 2); // truncated
    }
}

double processId() {
    return static_cast<double>(GetCurrentProcessId());
}

#else // POSIX

char** environBlock() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

void setEnvironBlock(char** block) {
#if defined(__APPLE__)
    *_NSGetEnviron() = block;
#else
    environ = block;
#endif
}

bool getEnv(const char* name, std::string& value) {
    const char* found = std::getenv(name);
    if (!found) {
        return false;
    }
    value = found;
    return true;
}

// value == nullptr removes the variable.
bool setEnv(const char* name, const char* value, std::string& error) {
    const int result = value ? ::setenv(name, value, 1) : ::unsetenv(name);
    if (result == 0) {
        return true;
    }
    error = errorText(errno);
    return false;
}

void listEnv(EnvList& out) {
    char** block = environBlock();
    for (char** entry = block; entry && *entry; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (!equals || equals == *entry) {
            continue;
        }
        out.emplace_back(std::string(*entry, static_cast<std::size_t>(equals - *entry)), std::string(equals + 1));
    }
}

bool currentDir(std::string& path, std::string& error) {
    std::vector<char> buffer(256);

    for (;;) {
        if (::getcwd(buffer.data(), buffer.size())) {
            path = buffer.data();
            return true;
        }
        if (errno != ERANGE) {
            error = errorText(errno);
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool changeDir(const char* path, std::string& error) {
    if (::chdir(path) == 0) {
        return true;
    }
    error = errorText(errno);
    return false;
}

#if defined(__linux__)

bool executablePath(std::string& path) {
    std::vector<char> buffer(256);

    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0) {
            return false;
        }
        if (static_cast<std::size_t>(n) < buffer.size()) {
            path.assign(buffer.data(), static_cast<std::size_t>(n));
            return true;
        }
        buffer.resize(buffer.size() * 2); // possibly truncated
    }
}

#elif defined(__APPLE__)

bool executablePath(std::string& path) {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // fails, but reports the size needed

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return false;
    }
    path = buffer.data();
    return true;
}

#else

bool executablePath(std::string&) {
    return false;
}

#endif

double processId() {
    return static_cast<double>(::getpid());
}

#endif // _WIN32

// running a child process ////////

struct ExecSpec {
    std::string program;
    std::vector<std::string> args;
    std::optional<std::string> cwd;
    EnvList env;                 // added to / overriding the inherited environment
    std::string stdinData;
    bool hasStdin = false;
    bool shell = false;
    bool inheritStdio = false;
};

struct ExecOutcome {
    bool launched = false;
    std::string error;           // why it could not be launched
    int code = 0;
    std::string out;
    std::string err;
};

#if defined(_WIN32)

class Handle {
public:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// Creates a pipe whose child end is inheritable and whose parent end is not.
bool makePipe(Handle& parentEnd, Handle& childEnd, bool childReads) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) {
        return false;
    }

    parentEnd.reset(childReads ? writeEnd : readEnd);
    childEnd.reset(childReads ? readEnd : writeEnd);
    return SetHandleInformation(parentEnd.get(), HANDLE_FLAG_INHERIT, 0) != 0;
}

std::string readAll(HANDLE handle) {
    std::string data;
    char buffer[16384];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        data.append(buffer, got);
    }
    return data;
}

void writeAll(HANDLE handle, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - written, 1u << 20));
        if (!WriteFile(handle, data.data() + written, want, &chunk, nullptr) || chunk == 0) {
            return; // the child closed its stdin
        }
        written += chunk;
    }
}

// Quotes one argument the way CommandLineToArgvW / the MSVC runtime will read it back.
std::wstring quoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(c);
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\'); // they precede the closing quote
    quoted.push_back(L'"');
    return quoted;
}

ExecOutcome runExec(const ExecSpec& spec) {
    ExecOutcome outcome;
    std::fflush(nullptr); // so our buffered output cannot appear after the child's

    std::wstring commandLine;
    if (spec.shell) {
        std::string command = spec.program;
        for (const std::string& arg : spec.args) {
            command += ' ';
            command += arg;
        }

        std::string comspec = "cmd.exe";
        std::string fromEnv;
        if (getEnv("ComSpec", fromEnv) && !fromEnv.empty()) {
            comspec = fromEnv;
        }
        commandLine = quoteArgument(widen(comspec)) + L" /d /s /c \"" + widen(command) + L"\"";
    } else {
        commandLine = quoteArgument(widen(spec.program));
        for (const std::string& arg : spec.args) {
            commandLine += L' ';
            commandLine += quoteArgument(widen(arg));
        }
    }

    // Environment block: current variables with the overrides applied, sorted as Windows wants.
    std::wstring environmentBlock;
    if (!spec.env.empty()) {
        WideEnvList merged = currentEnvironmentWide();
        for (const auto& [name, value] : spec.env) {
            const std::wstring wideName = widen(name);
            bool replaced = false;
            for (auto& existing : merged) {
                if (sameName(existing.first, wideName)) {
                    existing.second = widen(value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.emplace_back(wideName, widen(value));
            }
        }

        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });

        for (const auto& [name, value] : merged) {
            environmentBlock += name;
            environmentBlock += L'=';
            environmentBlock += value;
            environmentBlock += L'\0';
        }
        environmentBlock += L'\0';
    }

    const std::wstring workingDir = spec.cwd ? widen(*spec.cwd) : std::wstring();

    Handle inParent, inChild, outParent, outChild, errParent, errChild;
    if (!spec.inheritStdio) {
        if (!makePipe(inParent, inChild, true) || !makePipe(outParent, outChild, false) ||
            !makePipe(errParent, errChild, false)) {
            outcome.error = "failed to create pipes: " + errorText(static_cast<int>(GetLastError()));
            return outcome;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = spec.inheritStdio ? GetStdHandle(STD_INPUT_HANDLE) : inChild.get();
    startup.hStdOutput = spec.inheritStdio ? GetStdHandle(STD_OUTPUT_HANDLE) : outChild.get();
    startup.hStdError = spec.inheritStdio ? GetStdHandle(STD_ERROR_HANDLE) : errChild.get();

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE, // inherit the (inheritable) handles prepared above
        CREATE_UNICODE_ENVIRONMENT,
        spec.env.empty() ? nullptr : environmentBlock.data(),
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &startup,
        &info);

    if (!created) {
        outcome.error = "failed to run '" + spec.program + "': " + errorText(static_cast<int>(GetLastError()));
        return outcome;
    }

    CloseHandle(info.hThread);
    Handle process;
    process.reset(info.hProcess);

    // The child owns its ends now; keeping ours open would stop EOF from ever arriving.
    inChild.reset();
    outChild.reset();
    errChild.reset();

    if (!spec.inheritStdio) {
        std::thread errReader([&] { outcome.err = readAll(errParent.get()); });
        std::thread inWriter;

        if (spec.hasStdin && !spec.stdinData.empty()) {
            inWriter = std::thread([&] {
                writeAll(inParent.get(), spec.stdinData);
                inParent.reset();
            });
        } else {
            inParent.reset();
        }

        outcome.out = readAll(outParent.get());

        errReader.join();
        if (inWriter.joinable()) {
            inWriter.join();
        }
    }

    WaitForSingleObject(process.get(), INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.get(), &exitCode);

    outcome.launched = true;
    outcome.code = static_cast<int>(exitCode);
    return outcome;
}

#else // POSIX

class Fd {
public:
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Takes a descriptor out of the 0-2 range (so a later dup2 onto 0-2 cannot clobber
// it) and marks it close-on-exec.
bool secureFd(Fd& fd) {
    if (!fd.valid()) {
        return false;
    }
    const int moved = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
    if (moved < 0) {
        return false;
    }
    fd.reset(moved);
    return true;
}

bool makePipe(Fd& readEnd, Fd& writeEnd) {
    int fds[2];
    if (::pipe(fds) != 0) {
        return false;
    }
    readEnd.reset(fds[0]);
    writeEnd.reset(fds[1]);
    return secureFd(readEnd) && secureFd(writeEnd);
}

struct ChildFailure {
    int stage; // 0: stdio setup, 1: chdir, 2: exec
    int error;
};

[[noreturn]] void childFail(int reportFd, int stage, int error) {
    const ChildFailure failure{stage, error};
    const ssize_t ignored = ::write(reportFd, &failure, sizeof(failure));
    (void)ignored;
    ::_exit(127);
}

// Keeps SIGPIPE from killing us when the child closes its stdin before we are done writing.
class SigpipeBlocker {
public:
    SigpipeBlocker() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        active_ = pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0 && !sigismember(&previous_, SIGPIPE);
    }

    ~SigpipeBlocker() {
        if (active_) {
            sigset_t pending;
            sigemptyset(&pending);
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
                int ignored = 0;
                sigwait(&set_, &ignored); // consume it so it is not delivered once unblocked
            }
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        }
    }

    SigpipeBlocker(const SigpipeBlocker&) = delete;
    SigpipeBlocker& operator=(const SigpipeBlocker&) = delete;

private:
    sigset_t set_;
    sigset_t previous_;
    bool active_ = false;
};

// Feeds "input" to the child while draining its output, so a full pipe in either
// direction can never deadlock us. Closes all three descriptors when done.
void pump(const std::string& input, Fd& inWrite, Fd& outRead, Fd& errRead, std::string& out, std::string& err) {
    std::size_t written = 0;

    if (inWrite.valid()) {
        if (input.empty()) {
            inWrite.reset(); // the child sees EOF straight away
        } else {
            const int flags = ::fcntl(inWrite.get(), F_GETFL);
            ::fcntl(inWrite.get(), F_SETFL, flags | O_NONBLOCK);
        }
    }

    char buffer[16384];

    while (inWrite.valid() || outRead.valid() || errRead.valid()) {
        pollfd fds[3];
        int kinds[3];
        nfds_t count = 0;

        if (inWrite.valid()) {
            fds[count] = {inWrite.get(), POLLOUT, 0};
            kinds[count++] = 0;
        }
        if (outRead.valid()) {
            fds[count] = {outRead.get(), POLLIN, 0};
            kinds[count++] = 1;
        }
        if (errRead.valid()) {
            fds[count] = {errRead.get(), POLLIN, 0};
            kinds[count++] = 2;
        }

        if (::poll(fds, count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].revents == 0) {
                continue;
            }

            if (kinds[i] == 0) {
                const ssize_t n = ::write(inWrite.get(), input.data() + written, input.size() - written);
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                }
                const bool failed = n < 0 && errno != EAGAIN && errno != EINTR;
                if (failed || written == input.size()) {
                    inWrite.reset();
                }
            } else {
                Fd& source = kinds[i] == 1 ? outRead : errRead;
                std::string& sink = kinds[i] == 1 ? out : err;

                const ssize_t n = ::read(source.get(), buffer, sizeof(buffer));
                if (n > 0) {
                    sink.append(buffer, static_cast<std::size_t>(n));
                } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    source.reset();
                }
            }
        }
    }

    inWrite.reset();
    outRead.reset();
    errRead.reset();
}

int reap(pid_t pid) {
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return status;
}

ExecOutcome runExec(const ExecSpec& spec) {
    ExecOutcome outcome;
    std::fflush(nullptr); // so our buffered output cannot appear after the child's

    // Everything the child needs is prepared here: after fork() it may not allocate.
    std::vector<std::string> words;
    if (spec.shell) {
        std::string command = spec.program;
        for (const std::string& arg : spec.args) {
            command += ' ';
            command += arg;
        }
        words.push_back("/bin/sh");
        words.push_back("-c");
        words.push_back(std::move(command));
    } else {
        words.push_back(spec.program);
        words.insert(words.end(), spec.args.begin(), spec.args.end());
    }

    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    std::vector<char*> envp;
    const bool customEnv = !spec.env.empty();
    if (customEnv) {
        EnvList merged;
        listEnv(merged);

        for (const auto& [name, value] : spec.env) {
            bool replaced = false;
            for (auto& existing : merged) {
                if (existing.first == name) {
                    existing.second = value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.emplace_back(name, value);
            }
        }

        for (const auto& [name, value] : merged) {
            environment.push_back(name + "=" + value);
        }
        for (std::string& entry : environment) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }

    const char* cwd = spec.cwd ? spec.cwd->c_str() : nullptr;

    Fd inRead, inWrite, outRead, outWrite, errRead, errWrite, failRead, failWrite;
    if (!spec.inheritStdio) {
        if (!makePipe(inRead, inWrite) || !makePipe(outRead, outWrite) || !makePipe(errRead, errWrite)) {
            outcome.error = "failed to create pipes: " + errorText(errno);
            return outcome;
        }
    }
    if (!makePipe(failRead, failWrite)) {
        outcome.error = "failed to create pipes: " + errorText(errno);
        return outcome;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        outcome.error = "failed to fork: " + errorText(errno);
        return outcome;
    }

    if (pid == 0) {
        // Child. Only async-signal-safe calls from here until exec; it never returns.
        if (!spec.inheritStdio) {
            if (::dup2(inRead.get(), 0) < 0 || ::dup2(outWrite.get(), 1) < 0 || ::dup2(errWrite.get(), 2) < 0) {
                childFail(failWrite.get(), 0, errno);
            }
        }
        if (cwd && ::chdir(cwd) != 0) {
            childFail(failWrite.get(), 1, errno);
        }
        if (customEnv) {
            setEnvironBlock(envp.data());
        }
        ::execvp(argv[0], argv.data());
        childFail(failWrite.get(), 2, errno);
    }

    // Parent: drop the child's ends so EOF can be seen, then learn whether exec worked
    // (the report pipe closes on a successful exec, thanks to close-on-exec).
    inRead.reset();
    outWrite.reset();
    errWrite.reset();
    failWrite.reset();

    ChildFailure failure{0, 0};
    ssize_t got;
    do {
        got = ::read(failRead.get(), &failure, sizeof(failure));
    } while (got < 0 && errno == EINTR);
    failRead.reset();

    if (got == static_cast<ssize_t>(sizeof(failure))) {
        reap(pid);
        if (failure.stage == 1) {
            outcome.error = "failed to change directory to '" + *spec.cwd + "': " + errorText(failure.error);
        } else if (failure.stage == 2) {
            outcome.error = "failed to run '" + spec.program + "': " + errorText(failure.error);
        } else {
            outcome.error = "failed to set up the child's stdio: " + errorText(failure.error);
        }
        return outcome;
    }

    if (spec.inheritStdio) {
        // nothing to pump
    } else if (spec.hasStdin && !spec.stdinData.empty()) {
        SigpipeBlocker blocker;
        pump(spec.stdinData, inWrite, outRead, errRead, outcome.out, outcome.err);
    } else {
        pump(std::string(), inWrite, outRead, errRead, outcome.out, outcome.err);
    }

    const int status = reap(pid);
    outcome.launched = true;
    if (WIFEXITED(status)) {
        outcome.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.code = 128 + WTERMSIG(status);
    } else {
        outcome.code = -1;
    }
    return outcome;
}

#endif // _WIN32

} // namespace sys

// bindings ////////////
//
// Same discipline as example.cpp: validate every argument first (these checks hold
// no C++ objects), then do the real work, and raise failures from the platform layer
// only once the objects that carried them are gone.

bool hasNoNul(const char* text, std::size_t length) {
    return std::strlen(text) == length;
}

// process.cwd(): string
int processCwd(lua_State* L) {
    {
        std::string path;
        std::string error;
        if (sys::currentDir(path, error)) {
            lua_pushlstring(L, path.data(), path.size());
            return 1;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot determine the working directory: %s", lua_tostring(L, -1));
}

// process.chdir(path: string)
int processChdir(lua_State* L) {
    std::size_t length = 0;
    const char* path = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length != 0 && hasNoNul(path, length), 1, "path must be a non-empty string without null bytes");

    {
        std::string error;
        if (sys::changeDir(path, error)) {
            return 0;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot change directory to '%s': %s", path, lua_tostring(L, -1));
}

// process.exit(code: number?)
int processExit(lua_State* L) {
    const int code = luaL_optinteger(L, 1, 0);
    std::exit(code);
}

// ---- process.env ----

// process.env.NAME: string?
int envIndex(lua_State* L) {
    std::size_t length = 0;
    const char* name = nullptr;

    if (lua_type(L, 2) == LUA_TSTRING) {
        name = lua_tolstring(L, 2, &length);
    }

    if (name && length != 0 && hasNoNul(name, length)) {
        std::string value;
        if (sys::getEnv(name, value)) {
            lua_pushlstring(L, value.data(), value.size());
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

// process.env.NAME = value: string?
int envNewIndex(lua_State* L) {
    luaL_argcheck(L, lua_type(L, 2) == LUA_TSTRING, 2, "environment variable names must be strings");

    std::size_t nameLength = 0;
    const char* name = lua_tolstring(L, 2, &nameLength);
    luaL_argcheck(L, nameLength != 0 && hasNoNul(name, nameLength) && !std::strchr(name, '='), 2,
        "environment variable names must be non-empty and contain no '=' or null bytes");

    const char* value = nullptr;
    if (!lua_isnil(L, 3)) {
        luaL_argcheck(L, lua_type(L, 3) == LUA_TSTRING, 3, "environment variable values must be strings or nil");

        std::size_t valueLength = 0;
        value = lua_tolstring(L, 3, &valueLength);
        luaL_argcheck(L, hasNoNul(value, valueLength), 3, "environment variable values must not contain null bytes");
    }

    {
        std::string error;
        if (sys::setEnv(name, value, error)) {
            return 0;
        }
        lua_pushlstring(L, error.data(), error.size());
    }
    luaL_error(L, "cannot set environment variable '%s': %s", name, lua_tostring(L, -1));
}

// The iterator behind "for name, value in process.env": walks a snapshot taken
// when the loop started, kept in upvalue 1.
int envIterNext(lua_State* L) {
    lua_settop(L, 2);                          // (state, control); the control is the previous name
    lua_pushvalue(L, lua_upvalueindex(1));     // snapshot
    lua_pushvalue(L, 2);
    if (lua_next(L, -2) != 0) {
        return 2;                              // name, value
    }
    return 0;                                  // finished
}

// __iter: returns the iterator function (state and control start as nil).
int envIter(lua_State* L) {
    lua_createtable(L, 0, 32);
    {
        sys::EnvList vars;
        sys::listEnv(vars);

        for (const auto& [name, value] : vars) {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, name.c_str());
        }
    }

    lua_pushcclosure(L, envIterNext, "next", 1); // takes the snapshot as its upvalue
    return 1;
}

// A table with no entries of its own whose accesses all go to the real environment.
void pushEnvironmentProxy(lua_State* L) {
    lua_createtable(L, 0, 0);

    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, envIndex, "env.__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, envNewIndex, "env.__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, envIter, "env.__iter");
    lua_setfield(L, -2, "__iter");
    lua_pushstring(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");

    lua_setmetatable(L, -2);
}

// ---- process.exec ----

// Raises an argument error unless options[field] is nil or a string. Holds no C++ objects.
void checkOptionalString(lua_State* L, int options, const char* field) {
    lua_getfield(L, options, field);
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING) {
        luaL_error(L, "invalid option '%s' (string expected, got %s)", field, luaL_typename(L, -1));
    }
    lua_pop(L, 1);
}

void checkExecArguments(lua_State* L) {
    std::size_t length = 0;
    const char* program = luaL_checklstring(L, 1, &length);
    luaL_argcheck(L, length != 0, 1, "program must not be empty");
    luaL_argcheck(L, hasNoNul(program, length), 1, "program must not contain null bytes");

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);

        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 2, "args must be an array of strings");

            const char* arg = lua_tolstring(L, -1, &length);
            luaL_argcheck(L, hasNoNul(arg, length), 2, "args must not contain null bytes");
            lua_pop(L, 1);
        }
    }

    if (lua_isnoneornil(L, 3)) {
        return;
    }
    luaL_checktype(L, 3, LUA_TTABLE);

    checkOptionalString(L, 3, "cwd");
    checkOptionalString(L, 3, "stdin");

    lua_getfield(L, 3, "cwd");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* cwd = lua_tolstring(L, -1, &length);
        luaL_argcheck(L, length != 0 && hasNoNul(cwd, length), 3, "option 'cwd' must be a non-empty string without null bytes");
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "shell");
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN) {
        luaL_error(L, "invalid option 'shell' (boolean expected, got %s)", luaL_typename(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdio");
    if (!lua_isnil(L, -1)) {
        const char* mode = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        luaL_argcheck(L, mode && (std::strcmp(mode, "pipe") == 0 || std::strcmp(mode, "inherit") == 0), 3,
            "option 'stdio' must be \"pipe\" or \"inherit\"");
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "env");
    if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_istable(L, -1), 3, "option 'env' must be a table of strings");

        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            luaL_argcheck(L, lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING, 3,
                "option 'env' must map strings to strings");

            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            luaL_argcheck(L, nameLength != 0 && hasNoNul(name, nameLength) && !std::strchr(name, '='), 3,
                "option 'env' has an invalid variable name");
            luaL_argcheck(L, hasNoNul(value, valueLength), 3, "option 'env' values must not contain null bytes");

            lua_pop(L, 1); // keep the key for lua_next
        }
    }
    lua_pop(L, 1);
}

// Copies the (already validated) arguments into C++ objects. Raises nothing.
sys::ExecSpec readExecSpec(lua_State* L) {
    sys::ExecSpec spec;
    std::size_t length = 0;

    const char* program = lua_tolstring(L, 1, &length);
    spec.program.assign(program, length);

    if (lua_istable(L, 2)) {
        const int count = lua_objlen(L, 2);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            const char* arg = lua_tolstring(L, -1, &length);
            spec.args.emplace_back(arg, length);
            lua_pop(L, 1);
        }
    }

    if (!lua_istable(L, 3)) {
        return spec;
    }

    lua_getfield(L, 3, "cwd");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* cwd = lua_tolstring(L, -1, &length);
        spec.cwd = std::string(cwd, length);
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdin");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char* data = lua_tolstring(L, -1, &length);
        spec.stdinData.assign(data, length);
        spec.hasStdin = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "shell");
    spec.shell = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 3, "stdio");
    if (lua_type(L, -1) == LUA_TSTRING) {
        spec.inheritStdio = std::strcmp(lua_tostring(L, -1), "inherit") == 0;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "env");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            std::size_t nameLength = 0;
            std::size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            spec.env.emplace_back(std::string(name, nameLength), std::string(value, valueLength));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    return spec;
}

// process.exec(program: string, args: { string }?, options: ExecOptions?): ExecResult
int processExec(lua_State* L) {
    checkExecArguments(L);

    {
        const sys::ExecOutcome outcome = sys::runExec(readExecSpec(L));

        if (outcome.launched) {
            lua_createtable(L, 0, 4);

            lua_pushboolean(L, outcome.code == 0);
            lua_setfield(L, -2, "ok");
            lua_pushnumber(L, outcome.code);
            lua_setfield(L, -2, "code");
            lua_pushlstring(L, outcome.out.data(), outcome.out.size());
            lua_setfield(L, -2, "stdout");
            lua_pushlstring(L, outcome.err.data(), outcome.err.size());
            lua_setfield(L, -2, "stderr");
            return 1;
        }

        lua_pushlstring(L, outcome.error.data(), outcome.error.size());
    }
    luaL_error(L, "%s", lua_tostring(L, -1));
}

constexpr NativeFunction kFunctions[] = {
    {"cwd", processCwd},
    {"chdir", processChdir},
    {"exit", processExit},
    {"exec", processExec},
};

} // namespace

// The opener: runs once per VM and returns the library table.
void openProcess(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 8);
    setFunctions(L, kFunctions);

    lua_pushstring(L, sys::osName());
    lua_setfield(L, -2, "os");

    lua_pushstring(L, sys::archName());
    lua_setfield(L, -2, "arch");

    lua_pushstring(L, sys::endiannessName());
    lua_setfield(L, -2, "endianness");

    lua_pushnumber(L, sys::processId());
    lua_setfield(L, -2, "pid");

    {
        std::string path;
        if (sys::executablePath(path)) {
            lua_pushlstring(L, path.data(), path.size());
            lua_setfield(L, -2, "execPath");
        }
    }

    // The host's argument list, if it provided one.
    lua_getfield(L, LUA_REGISTRYINDEX, kArgsRegistryKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 0);
    }
    lua_setfield(L, -2, "args");

    pushEnvironmentProxy(L);
    lua_setfield(L, -2, "env");
}

// host API implementation //////////
namespace process {

void setArguments(lua_State* L, const std::vector<std::string>& args) {
    lua_createtable(L, static_cast<int>(args.size()), 0);
    for (std::size_t i = 0; i < args.size(); ++i) {
        lua_pushlstring(L, args[i].data(), args[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, kArgsRegistryKey);
}

} // namespace process

} // namespace sonata::lib::libs
