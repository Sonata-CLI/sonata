// @sonata/sys: facts about the machine the script runs on, plus clocks and sleeping.
// Queries that can fail answer nil instead of raising. It does not depend on any other library.
//
//     local sys = require("@sonata/sys")
//
//     sys.eol                 --> "\n"          ("\r\n" on Windows)
//     sys.pathSeparator       --> "/"           ("\\" on Windows)
//     sys.pathDelimiter       --> ":"           (";" on Windows), as used in PATH
//     sys.devNull             --> "/dev/null"   ("NUL" on Windows)
//
//     sys.hostname()          --> string?
//     sys.username()          --> string?
//     sys.homeDir()           --> string?
//     sys.tempDir()           --> string?
//     sys.osVersion()         --> string?       OS/kernel release, e.g. "6.8.0-45-generic"
//     sys.cpuCount()          --> number        logical processors, at least 1
//     sys.totalMemory()       --> number?       bytes of physical memory
//     sys.freeMemory()        --> number?       bytes available to programs
//     sys.uptime()            --> number?       seconds since the machine booted
//     sys.time()              --> number        wall clock: seconds since the Unix epoch (fractional)
//     sys.clock()             --> number        monotonic seconds from an arbitrary start; use it to time things
//     sys.sleep(seconds)                        blocks the whole VM (use task.wait to let other tasks run)
//     sys.osName()            --> string        the name of the current operating system
//     sys.arch()              --> string        system architecture
//     sys.pid()               --> number        the PID of the process
//     sys.info()              --> table         every query above in one table (hostname, username, homeDir,
//                                               tempDir, osVersion, cpuCount, totalMemory, freeMemory, uptime)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#endif

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

using OptString = std::optional<std::string>;
using OptNumber = std::optional<double>;

// platform layer //////////////

#if defined(_WIN32)

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

OptString envVar(const wchar_t* name) {
    wchar_t buffer[32768];
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return std::nullopt;
    }
    return narrow(buffer, n);
}

constexpr const char* kEol = "\r\n";
constexpr const char* kPathSeparator = "\\";
constexpr const char* kPathDelimiter = ";";
constexpr const char* kDevNull = "NUL";

OptString hostname() {
    wchar_t buffer[256];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameExW(ComputerNameDnsHostname, buffer, &size)) {
        return std::nullopt;
    }
    return narrow(buffer, size);
}

OptString username() {
    wchar_t buffer[257];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetUserNameW(buffer, &size) || size == 0) {
        return std::nullopt;
    }
    return narrow(buffer, size - 1); // size counts the terminator
}

OptString homeDir() {
    return envVar(L"USERPROFILE");
}

OptString tempDir() {
    wchar_t buffer[MAX_PATH + 2];
    DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (n == 0 || n >= std::size(buffer)) {
        return std::nullopt;
    }
    if (n > 3 && (buffer[n - 1] == L'\\' || buffer[n - 1] == L'/')) {
        --n; // drop the trailing separator, but keep "C:\"
    }
    return narrow(buffer, n);
}

OptString osVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return std::nullopt;
    }
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!fn || fn(&info) != 0) {
        return std::nullopt;
    }
    return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber);
}

OptString osName() {
    return std::string("windows");
}

OptString arch() {
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return std::string("x86_64");
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return std::string("aarch64");
    if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return std::string("x86");
    return std::string("unknown");
}

OptNumber pid() {
    return static_cast<double>(GetCurrentProcessId());
}

OptNumber pageSize() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<double>(info.dwPageSize);
}


OptNumber cpuCount() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<double>(std::max<DWORD>(info.dwNumberOfProcessors, 1));
}

OptNumber totalMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return std::nullopt;
    }
    return static_cast<double>(status.ullTotalPhys);
}

OptNumber freeMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return std::nullopt;
    }
    return static_cast<double>(status.ullAvailPhys);
}

OptNumber uptime() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

#else // POSIX

constexpr const char* kEol = "\n";
constexpr const char* kPathSeparator = "/";
constexpr const char* kPathDelimiter = ":";
constexpr const char* kDevNull = "/dev/null";

OptString envVar(const char* name) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

// The passwd entry of the current user, if the system has one.
bool passwdEntry(std::string& name, std::string& dir) {
    passwd entry;
    passwd* result = nullptr;
    std::vector<char> buffer(4096);

    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || !result) {
        return false;
    }
    name = result->pw_name ? result->pw_name : "";
    dir = result->pw_dir ? result->pw_dir : "";
    return true;
}

OptString hostname() {
    char buffer[256];
    if (::gethostname(buffer, sizeof(buffer)) != 0) {
        return std::nullopt;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

OptString username() {
    std::string name, dir;
    if (passwdEntry(name, dir) && !name.empty()) {
        return name;
    }
    if (OptString fromEnv = envVar("USER")) {
        return fromEnv;
    }
    return envVar("LOGNAME");
}

OptString homeDir() {
    if (OptString fromEnv = envVar("HOME")) {
        return fromEnv;
    }
    std::string name, dir;
    if (passwdEntry(name, dir) && !dir.empty()) {
        return dir;
    }
    return std::nullopt;
}

OptString tempDir() {
    if (OptString fromEnv = envVar("TMPDIR")) {
        return fromEnv;
    }
    return std::string("/tmp");
}

OptString osVersion() {
    utsname info;
    if (::uname(&info) != 0) {
        return std::nullopt;
    }
    return std::string(info.release);
}

OptString osName() {
#if defined(__APPLE__)
    return std::string("macos");
#elif defined(__linux__)
    return std::string("linux");
#else
    return std::string("posix");
#endif
}

OptString arch() {
    utsname info;
    if (::uname(&info) != 0) {
        return std::nullopt;
    }
    std::string m = info.machine;
    if (m == "amd64") return std::string("x86_64");
    if (m == "arm64") return std::string("aarch64");
    return m;
}

OptNumber pid() {
    return static_cast<double>(::getpid());
}

OptNumber pageSize() {
    const long size = ::sysconf(_SC_PAGESIZE);
    if (size <= 0) {
        return std::nullopt;
    }
    return static_cast<double>(size);
}


OptNumber cpuCount() {
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    return static_cast<double>(count > 0 ? count : 1);
}

#if defined(__linux__)

OptNumber totalMemory() {
    struct sysinfo info;
    if (::sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<double>(info.totalram) * static_cast<double>(info.mem_unit);
}

OptNumber freeMemory() {
    // MemAvailable counts reclaimable caches; plain "free" RAM would understate what programs can use.
    if (std::FILE* file = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        unsigned long long kilobytes = 0;
        while (std::fgets(line, sizeof(line), file)) {
            if (std::sscanf(line, "MemAvailable: %llu kB", &kilobytes) == 1) {
                std::fclose(file);
                return static_cast<double>(kilobytes) * 1024.0;
            }
        }
        std::fclose(file);
    }

    struct sysinfo info;
    if (::sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<double>(info.freeram) * static_cast<double>(info.mem_unit);
}

OptNumber uptime() {
    struct sysinfo info;
    if (::sysinfo(&info) != 0) {
        return std::nullopt;
    }
    return static_cast<double>(info.uptime);
}

#elif defined(__APPLE__)

OptNumber totalMemory() {
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    if (::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) {
        return std::nullopt;
    }
    return static_cast<double>(bytes);
}

OptNumber freeMemory() {
    vm_statistics64_data_t stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) !=
        KERN_SUCCESS) {
        return std::nullopt;
    }
    const double pages = static_cast<double>(stats.free_count) + static_cast<double>(stats.inactive_count);
    return pages * static_cast<double>(vm_kernel_page_size);
}

OptNumber uptime() {
    timeval boot;
    std::size_t size = sizeof(boot);
    if (::sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) != 0) {
        return std::nullopt;
    }
    timeval now;
    ::gettimeofday(&now, nullptr);
    return static_cast<double>(now.tv_sec - boot.tv_sec);
}

#else // other POSIX systems: best effort

OptNumber totalMemory() {
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) {
        return std::nullopt;
    }
    return static_cast<double>(pages) * static_cast<double>(pageSize);
}

OptNumber freeMemory() {
#if defined(_SC_AVPHYS_PAGES)
    const long pages = ::sysconf(_SC_AVPHYS_PAGES);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<double>(pages) * static_cast<double>(pageSize);
    }
#endif
    return std::nullopt;
}

OptNumber uptime() {
    return std::nullopt;
}

#endif // platform memory/uptime

#endif // _WIN32

double wallClock() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

double monotonicClock() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// bindings ///////////////////////////////

void pushOptional(lua_State* L, const OptString& value) {
    if (value) {
        lua_pushlstring(L, value->data(), value->size());
    } else {
        lua_pushnil(L);
    }
}

void pushOptional(lua_State* L, const OptNumber& value) {
    if (value) {
        lua_pushnumber(L, *value);
    } else {
        lua_pushnil(L);
    }
}

// Wraps a platform getter as a Lua function taking no arguments.
template <auto Getter>
int query(lua_State* L) {
    pushOptional(L, Getter());
    return 1;
}

template <auto Getter>
void setInfoField(lua_State* L, const char* key) {
    pushOptional(L, Getter());
    lua_setfield(L, -2, key);
}

// sys.time(): number
int sysTime(lua_State* L) {
    lua_pushnumber(L, wallClock());
    return 1;
}

// sys.clock(): number
int sysClock(lua_State* L) {
    lua_pushnumber(L, monotonicClock());
    return 1;
}

// sys.sleep(seconds: number)
int sysSleep(lua_State* L) {
    const double seconds = luaL_checknumber(L, 1);
    luaL_argcheck(L, !std::isnan(seconds), 1, "duration must not be NaN");

    if (seconds > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(seconds, 1.0e9)));
    }
    return 0;
}

// sys.info(): table
int sysInfo(lua_State* L) {
    lua_createtable(L, 0, 13); // Increased from 9 to accommodate new fields
    setInfoField<hostname>(L, "hostname");
    setInfoField<username>(L, "username");
    setInfoField<homeDir>(L, "homeDir");
    setInfoField<tempDir>(L, "tempDir");
    setInfoField<osName>(L, "osName");
    setInfoField<osVersion>(L, "osVersion");
    setInfoField<arch>(L, "arch");
    setInfoField<cpuCount>(L, "cpuCount");
    setInfoField<totalMemory>(L, "totalMemory");
    setInfoField<freeMemory>(L, "freeMemory");
    setInfoField<uptime>(L, "uptime");
    setInfoField<pid>(L, "pid");
    setInfoField<pageSize>(L, "pageSize");
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"hostname", query<hostname>},
    {"username", query<username>},
    {"homeDir", query<homeDir>},
    {"tempDir", query<tempDir>},
    {"osName", query<osName>},
    {"osVersion", query<osVersion>},
    {"arch", query<arch>},
    {"cpuCount", query<cpuCount>},
    {"totalMemory", query<totalMemory>},
    {"freeMemory", query<freeMemory>},
    {"uptime", query<uptime>},
    {"pid", query<pid>},
    {"pageSize", query<pageSize>},
    {"time", sysTime},
    {"clock", sysClock},
    {"sleep", sysSleep},
    {"info", sysInfo},
};

} // namespace

void openSys(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 4);
    setFunctions(L, kFunctions);

    lua_pushstring(L, kEol);
    lua_setfield(L, -2, "eol");
    lua_pushstring(L, kPathSeparator);
    lua_setfield(L, -2, "pathSeparator");
    lua_pushstring(L, kPathDelimiter);
    lua_setfield(L, -2, "pathDelimiter");
    lua_pushstring(L, kDevNull);
    lua_setfield(L, -2, "devNull");
}

} // namespace sonata::lib::libs
