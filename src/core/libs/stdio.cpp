// @sonata/stdio: the process's standard streams.
//
//     local stdio = require("@sonata/stdio")
//
// The three streams are objects, so the same code works for any of them:
//
//     stdio.stdin / stdio.stdout / stdio.stderr
//
//     stream:write(...)             --> stream?, string?   strings and numbers only; returns the stream so calls chain
//     stream:writeLine(...)         --> stream?, string?   like write, then a newline
//     stream:flush()                --> stream?, string?
//     stream:readLine(keepEnding)   --> string?, string?   next line, without its line ending unless keepEnding is
//                                                          true; nil at end of input
//     stream:read(count)            --> string?, string?   exactly "count" bytes (waits until they have all arrived);
//                                                          fewer only at EOF; nil at EOF
//     stream:readSome(maxCount)     --> string?, string?   up to "maxCount" bytes, returned as soon as at least one is
//                                                          available (waits for the first byte only); nil at EOF
//     stream:readAll()              --> string?, string?   everything until EOF ("" when already at EOF)
//     stream:lines(keepEnding)      --> iterator           for line in stdio.stdin:lines() do ... end
//     stream:isTerminal()           --> boolean
//     stream:size()                 --> columns, rows      (nil if the stream is not a terminal)
//     stdio.prompt("Name: ")        --> string?, string?   writes to stdout, flushes, then reads a line from stdin
//
// I/O failures never raise: every method that can fail returns nil and a message instead (so "nil" alone means
// end of input, "nil, message" means failure). Wrong arguments and using a stream in the wrong direction still
// raise, because those are programming mistakes: write methods only work on stdout / stderr and read methods only
// on stdin. A line ends at "\n", "\r\n" or a lone "\r".
//
// Output goes through C stdio, so it stays in order with print() and with child processes started by
// @sonata/process. Writing never flushes by itself: stdout follows the usual C buffering (line-buffered on a
// terminal), so call flush() when output must appear immediately. Input is read straight from the OS descriptor
// through one process-wide buffer instead of C's "stdin", so nothing else in the process should read C stdin;
// stdout is flushed before every blocking read so a prompt written with write() is visible.

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

constexpr const char* kStreamMetatable = "sonata.stdio.stream";
constexpr const char* kStreamNames[] = {"stdin", "stdout", "stderr"};

// Size of the single stdin buffer, allocated on first read. Requests of any size or lines of any length
// still work: they are assembled from as many refills as needed.
constexpr std::size_t kInputChunkSize = 64 * 1024;

// A stream object is a userdata holding its descriptor number: 0, 1 or 2.
int checkStream(lua_State* L) {
    return *static_cast<const int*>(luaL_checkudata(L, 1, kStreamMetatable));
}

int checkWritable(lua_State* L) {
    const int fd = checkStream(L);
    luaL_argcheck(L, fd != 0, 1, "stdin cannot be written to");
    return fd;
}

int checkReadable(lua_State* L) {
    const int fd = checkStream(L);
    luaL_argcheck(L, fd == 0, 1, "only stdin can be read from");
    return fd;
}

std::FILE* fileOf(int fd) {
    return fd == 0 ? stdin : (fd == 1 ? stdout : stderr);
}

// platform layer //////////////////////////////

bool isTerminal(int fd) {
#if defined(_WIN32)
    return _isatty(fd) != 0;
#else
    return ::isatty(fd) != 0;
#endif
}

bool terminalSize(int fd, int& columns, int& rows) {
#if defined(_WIN32)
    const DWORD which = fd == 0 ? STD_INPUT_HANDLE : (fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(which), &info)) {
        return false;
    }
    columns = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return true;
#else
    winsize size;
    if (::ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return false;
    }
    columns = size.ws_col;
    rows = size.ws_row;
    return true;
#endif
}

const char* describeError(int err) {
    return err != 0 ? std::strerror(err) : "unknown error";
}

// One read from the stdin descriptor: returns as soon as any data is available.
// Result: bytes read, 0 at EOF, -1 on error (errno set).
std::ptrdiff_t readOs(char* destination, std::size_t size) {
#if defined(_WIN32)
    return _read(0, destination, static_cast<unsigned>(std::min<std::size_t>(size, 1u << 30)));
#else
    for (;;) {
        const ssize_t got = ::read(0, destination, size);
        if (got >= 0 || errno != EINTR) {
            return got;
        }
    }
#endif
}

// stdin buffer ///////////////////////////////////////

struct StdinState {
    std::mutex mutex;
    std::vector<char> data; // allocated on first refill
    std::size_t pos = 0;    // next unread byte
    std::size_t len = 0;    // bytes currently held
};

StdinState& stdinState() {
    static StdinState state;
    return state;
}

enum class ReadResult { Data, Eof, Error };

// Makes sure at least one unread byte is buffered, blocking on the OS only if the buffer is empty.
// Returns false at EOF (err == 0) or on failure (err != 0). Caller holds the lock.
bool refill(StdinState& state, int& err) {
    err = 0;
    if (state.pos < state.len) {
        return true;
    }
    if (state.data.empty()) {
        state.data.resize(kInputChunkSize);
    }

    std::fflush(stdout); // a pending prompt must be visible before we wait for input
    const std::ptrdiff_t got = readOs(state.data.data(), state.data.size());

    state.pos = 0;
    state.len = got > 0 ? static_cast<std::size_t>(got) : 0;
    if (got < 0) {
        err = errno != 0 ? errno : EIO;
    }
    return got > 0;
}

// Reads one line. Data = a line (maybe empty), Eof = nothing left, Error = err is set.
ReadResult readLineFromStdin(std::string& line, bool keepEnding, int& err) {
    StdinState& state = stdinState();
    const std::lock_guard<std::mutex> guard(state.mutex);
    line.clear();

    for (;;) {
        if (!refill(state, err)) {
            if (err != 0) {
                return ReadResult::Error;
            }
            return line.empty() ? ReadResult::Eof : ReadResult::Data; // last line had no ending
        }

        const char* begin = state.data.data() + state.pos;
        const char* end = state.data.data() + state.len;
        const char* hit = std::find_if(begin, end, [](char c) { return c == '\n' || c == '\r'; });

        line.append(begin, hit);
        if (hit == end) {
            state.pos = state.len;
            continue;
        }

        state.pos += static_cast<std::size_t>(hit - begin) + 1;

        const bool carriageReturn = *hit == '\r';
        bool crlf = false;
        if (carriageReturn) {
            // A "\n" right behind it belongs to the same ending, even across a buffer refill.
            int ignored = 0;
            if (refill(state, ignored) && state.data[state.pos] == '\n') {
                ++state.pos;
                crlf = true;
            }
        }

        if (keepEnding) {
            if (carriageReturn) {
                line.push_back('\r');
            }
            if (!carriageReturn || crlf) {
                line.push_back('\n');
            }
        }
        return ReadResult::Data;
    }
}

// Reads up to "count" bytes. all = keep reading until "count" bytes or EOF; otherwise return after the first
// batch of bytes. Data = bytes (maybe empty when count is 0), Eof = nothing left, Error = err is set.
ReadResult readFromStdin(std::string& out, std::size_t count, bool all, int& err) {
    StdinState& state = stdinState();
    const std::lock_guard<std::mutex> guard(state.mutex);
    out.clear();

    while (out.size() < count) {
        if (!refill(state, err)) {
            if (err != 0) {
                return ReadResult::Error;
            }
            break; // EOF
        }

        const std::size_t take = std::min(count - out.size(), state.len - state.pos);
        out.append(state.data.data() + state.pos, take);
        state.pos += take;

        if (!all) {
            break;
        }
    }

    return (out.empty() && count > 0) ? ReadResult::Eof : ReadResult::Data;
}

// bindings //////////////////////////////////////////////
//
// As in example.cpp: arguments are validated first, and the writers below hold no C++
// objects at all, so a Lua error can never skip a destructor.

// Pushes "nil, "failed to <action> <stream>: <reason>"".
int pushFailure(lua_State* L, const char* action, int fd, int err) {
    lua_pushnil(L);
    lua_pushfstring(L, "failed to %s %s: %s", action, kStreamNames[fd], describeError(err));
    return 2;
}

// Pushes the next line from stdin, nil at EOF, or nil plus a message on failure.
int pushLine(lua_State* L, bool keepEnding) {
    std::string line;
    int err = 0;

    switch (readLineFromStdin(line, keepEnding, err)) {
    case ReadResult::Data:
        lua_pushlstring(L, line.data(), line.size());
        return 1;
    case ReadResult::Eof:
        lua_pushnil(L);
        return 1;
    default:
        return pushFailure(L, "read from", 0, err);
    }
}

int writeValues(lua_State* L, bool newline) {
    const int fd = checkWritable(L);
    const int top = lua_gettop(L);

    for (int i = 2; i <= top; ++i) {
        luaL_checklstring(L, i, nullptr); // numbers are converted in place
    }

    std::FILE* file = fileOf(fd);
    bool failed = false;
    int err = 0;

    for (int i = 2; i <= top && !failed; ++i) {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, i, &length);
        if (std::fwrite(text, 1, length, file) != length) {
            failed = true;
            err = errno;
        }
    }
    if (!failed && newline && std::fputc('\n', file) == EOF) {
        failed = true;
        err = errno;
    }
    if (failed) {
        std::clearerr(file); // leave the stream usable so the script can recover
        return pushFailure(L, "write to", fd, err);
    }

    lua_settop(L, 1);
    return 1; // the stream
}

// stream:write(...: string | number): stream?, string?
int streamWrite(lua_State* L) {
    return writeValues(L, false);
}

// stream:writeLine(...: string | number): stream?, string?
int streamWriteLine(lua_State* L) {
    return writeValues(L, true);
}

// stream:flush(): stream?, string?
int streamFlush(lua_State* L) {
    const int fd = checkWritable(L);
    std::FILE* file = fileOf(fd);

    if (std::fflush(file) != 0) {
        const int err = errno;
        std::clearerr(file);
        return pushFailure(L, "flush", fd, err);
    }

    lua_settop(L, 1);
    return 1;
}

// stream:readLine(keepEnding: boolean?): string?, string?
int streamReadLine(lua_State* L) {
    checkReadable(L);
    return pushLine(L, lua_toboolean(L, 2) != 0);
}

// Shared by read (exact) and readSome (whatever is available).
int readCount(lua_State* L, bool exact) {
    checkReadable(L);
    const int count = luaL_checkinteger(L, 2);
    luaL_argcheck(L, count >= 0, 2, "count must not be negative");

    std::string data;
    int err = 0;

    switch (readFromStdin(data, static_cast<std::size_t>(count), exact, err)) {
    case ReadResult::Data:
        lua_pushlstring(L, data.data(), data.size());
        return 1;
    case ReadResult::Eof:
        lua_pushnil(L);
        return 1;
    default:
        return pushFailure(L, "read from", 0, err);
    }
}

// stream:read(count: number): string?, string?
int streamRead(lua_State* L) {
    return readCount(L, true);
}

// stream:readSome(maxCount: number): string?, string?
int streamReadSome(lua_State* L) {
    return readCount(L, false);
}

// stream:readAll(): string?, string?
int streamReadAll(lua_State* L) {
    checkReadable(L);

    std::string data;
    int err = 0;

    if (readFromStdin(data, std::numeric_limits<std::size_t>::max(), true, err) == ReadResult::Error) {
        return pushFailure(L, "read from", 0, err);
    }

    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

// The iterator behind stream:lines(); upvalue 1 is keepEnding.
int linesNext(lua_State* L) {
    return pushLine(L, lua_toboolean(L, lua_upvalueindex(1)) != 0);
}

// stream:lines(keepEnding: boolean?): () -> string?
int streamLines(lua_State* L) {
    checkReadable(L);

    lua_pushboolean(L, lua_toboolean(L, 2));
    lua_pushcclosure(L, linesNext, "lines", 1);
    return 1;
}

// stream:isTerminal(): boolean
int streamIsTerminal(lua_State* L) {
    lua_pushboolean(L, isTerminal(checkStream(L)));
    return 1;
}

// stream:size(): (number, number)?
int streamSize(lua_State* L) {
    int columns = 0;
    int rows = 0;

    if (!terminalSize(checkStream(L), columns, rows)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, columns);
    lua_pushnumber(L, rows);
    return 2;
}

// tostring(stream): "stdio.stdout"
int streamToString(lua_State* L) {
    const std::string name = std::string("stdio.") + kStreamNames[checkStream(L)];
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

// stdio.prompt(message: string?): string?, string?
int stdioPrompt(lua_State* L) {
    std::size_t length = 0;
    const char* message = luaL_optlstring(L, 1, "", &length);

    if (std::fwrite(message, 1, length, stdout) != length || std::fflush(stdout) != 0) {
        const int err = errno;
        std::clearerr(stdout);
        return pushFailure(L, "write to", 1, err);
    }
    return pushLine(L, false);
}

constexpr NativeFunction kFunctions[] = {
    {"prompt", stdioPrompt},
};

constexpr NativeFunction kStreamMethods[] = {
    {"write", streamWrite},
    {"writeLine", streamWriteLine},
    {"flush", streamFlush},
    {"readLine", streamReadLine},
    {"read", streamRead},
    {"readSome", streamReadSome},
    {"readAll", streamReadAll},
    {"lines", streamLines},
    {"isTerminal", streamIsTerminal},
    {"size", streamSize},
};

} // namespace

void openStdio(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 3);
    setFunctions(L, kFunctions);

    if (luaL_newmetatable(L, kStreamMetatable)) {
        lua_createtable(L, 0, static_cast<int>(std::size(kStreamMethods)));
        setFunctions(L, kStreamMethods);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, streamToString, "stream.__tostring");
        lua_setfield(L, -2, "__tostring");

        lua_pushstring(L, "The metatable is locked");
        lua_setfield(L, -2, "__metatable");
    }

    for (int fd = 0; fd < 3; ++fd) {
        auto* slot = static_cast<int*>(lua_newuserdata(L, sizeof(int)));
        *slot = fd;
        lua_pushvalue(L, -2);
        lua_setmetatable(L, -2);
        lua_setfield(L, -3, kStreamNames[fd]);
    }
    lua_pop(L, 1); // the metatable!!!!
}

} // namespace sonata::lib::libs
