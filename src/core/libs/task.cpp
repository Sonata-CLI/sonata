// @sonata/task: cooperative scheduling, modelled on Roblox's "task" library.
//
//     local task = require("@sonata/task")
//
//     task.spawn(fn, ...)           --> thread   runs fn(...) right now, until it first yields
//     task.defer(fn, ...)           --> thread   runs fn(...) at the end of the current cycle
//     task.delay(seconds, fn, ...)  --> thread   runs fn(...) once "seconds" have passed
//     task.wait(seconds)            --> number   yields; resumes after "seconds" (default 0 = next
//                                                cycle) and returns how long it really waited
//     task.cancel(thread)                        stops a thread and drops everything scheduled for it
//
// Wherever "fn" is expected, a suspended coroutine works too: it is resumed with
// the extra arguments instead of a new thread being created.
//
// The library only queues work. The host runs it by pumping the scheduler (see the host
// API below) and must run the main script on a coroutine, otherwise task.wait()
// has nothing to yield.
//
// Differences from Roblox:
//   * task.synchronize / task.desynchronize do not exist (there is no Parallel Luau).
//   * There are no frames. One "cycle" is one task::step(): due timers first, then
//     the deferred threads queued so far (deferred threads also run once before the
//     timers, for work queued outside a cycle). A thread deferred from inside a
//     deferred thread waits for the next deferred phase.
//   * Errors in scheduled threads go to the host's error handler, not an output window.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

// host API //////////////////////////////////////////////
//
// Host-side control of the @sonata/task scheduler.
//
// The task library only *queues* work; nothing runs until the host pumps the
// scheduler. A minimal host looks like this:
//
//     lua_State* main = lua_newthread(L);   // run the script on a coroutine, otherwise
//     /* ...load the chunk onto "main"... */ // task.wait() cannot yield at top level
//     lua_resume(main, nullptr, 0);         // runs until the script ends or first yields
//     task::run(L);                         // resumes sleeping/deferred threads until none are left
//
// Hosts with their own event loop (IO, timers, ...) use step() and nextWakeup()
// instead of run(). Everything here is per-VM and safe to call before the
// library is first required (it simply finds nothing to do).
namespace sonata::lib::libs::task {

struct ThreadError {
    lua_State* thread;      // the thread that failed (dead; its stack is intact)
    std::string message;    // the error value; non-string errors are described by their type
    std::string traceback;  // "stack traceback:\n  ..." or empty when there are no frames
};

using ErrorHandler = std::function<void(const ThreadError&)>;

// Called when a thread started by the scheduler (or by task.spawn) raises an
// error. Without a handler the message and traceback go to stderr. Errors in
// threads the host resumes itself are the host's to deal with.
void setErrorHandler(lua_State* L, ErrorHandler handler);

// Seconds until the scheduler next has something to resume: 0 if work is
// runnable now, nullopt if there is nothing left. Threads sleeping for an
// infinite time (task.wait(math.huge)) do not count as work.
std::optional<double> nextWakeup(lua_State* L);

// Runs one cycle: deferred threads queued so far, then every timer that is due,
// then the threads those timers deferred. Returns true if work remains.
bool step(lua_State* L);

// Pumps step() to completion, sleeping the calling thread between timers.
void run(lua_State* L);

} // namespace sonata::lib::libs::task

namespace sonata::lib::libs {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kNever = std::numeric_limits<double>::infinity();
constexpr const char* kRegistryKey = "sonata.task.scheduler";
constexpr int kMaxTraceLevels = 32;
constexpr double kMaxSleepSeconds = 3600.0; // run() sleeps in chunks of at most this

// One queued resumption. It owns a registry reference that keeps "thread" alive
// while it waits; the reference is released when the entry is dispatched or
// cancelled.
struct Entry {
    lua_State* thread = nullptr; // nullptr once cancelled
    int ref = LUA_NOREF;
    int nargs = 0;               // values already on the thread's stack to resume it with
    bool isWait = false;         // task.wait: resume with the elapsed time instead
    double queuedAt = 0.0;       // task.wait: scheduler time when the wait began
    std::uint64_t seq = 0;       // creation order, see step()
};

// Per-VM state. It lives in a userdata anchored in the registry, so there are no
// C++ globals and lua_close() destroys it.
struct Scheduler {
    Clock::time_point origin = Clock::now();
    std::deque<Entry> deferred;            // task.defer, FIFO; cancelled entries stay as tombstones
    std::multimap<double, Entry> timers;   // task.delay / task.wait, keyed by wake time, FIFO on ties
    std::uint64_t nextSeq = 0;
    task::ErrorHandler onError;

    double now() const {
        return std::chrono::duration<double>(Clock::now() - origin).count();
    }
};

Scheduler* findScheduler(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    auto* scheduler = static_cast<Scheduler*>(lua_touserdata(L, -1)); // nullptr while the slot is nil
    lua_pop(L, 1);
    return scheduler;
}

Scheduler& schedulerOf(lua_State* L) {
    if (Scheduler* existing = findScheduler(L)) {
        return *existing;
    }

    void* memory = lua_newuserdatadtor(L, sizeof(Scheduler), [](lua_State*, void* data) {
        static_cast<Scheduler*>(data)->~Scheduler(); // no Lua calls here: the VM is going away
    });
    auto* scheduler = new (memory) Scheduler();
    lua_setfield(L, LUA_REGISTRYINDEX, kRegistryKey); // pops the userdata, anchoring it
    return *scheduler;
}

// error reporting ///////////////////////////////////////

// After a failed lua_resume the error value is on top of the dead thread's stack.
std::string describeError(lua_State* thread) {
    const int type = lua_type(thread, -1);
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        std::size_t length = 0;
        const char* text = lua_tolstring(thread, -1, &length);
        return std::string(text, length);
    }
    return std::string("<error value of type ") + luaL_typename(thread, -1) + ">";
}

// A failed coroutine keeps its call frames, so its traceback can still be read.
std::string describeTraceback(lua_State* thread) {
    std::string trace;
    lua_Debug ar;
    int level = 0;

    for (; level < kMaxTraceLevels && lua_getinfo(thread, level, "sln", &ar); ++level) {
        const char* source = ar.short_src;
        trace += "\n  ";
        trace += source ? source : "?";
        if (ar.currentline > 0) {
            trace += ':';
            trace += std::to_string(ar.currentline);
        }
        if (ar.name) {
            trace += " function ";
            trace += ar.name;
        }
    }

    if (level == 0) {
        return {};
    }
    if (level == kMaxTraceLevels) {
        trace += "\n  ...";
    }
    return "stack traceback:" + trace;
}

void reportError(Scheduler& scheduler, lua_State* thread) {
    task::ThreadError error{thread, describeError(thread), describeTraceback(thread)};

    if (scheduler.onError) {
        scheduler.onError(error);
        return;
    }

    std::fprintf(stderr, "%s\n", error.message.c_str());
    if (!error.traceback.empty()) {
        std::fprintf(stderr, "%s\n", error.traceback.c_str());
    }
}

// running threads /////////////////////////////////////////////////////

// Resumes "thread" with "nargs" values from its own stack. Errors never propagate
// to the caller: they are reported and the thread stays dead.
void resumeThread(Scheduler& scheduler, lua_State* thread, lua_State* from, int nargs) {
    const int status = lua_resume(thread, from, nargs);

    if (status == LUA_OK) {
        lua_settop(thread, 0); // finished: drop its return values
    } else if (status != LUA_YIELD && status != LUA_BREAK) {
        reportError(scheduler, thread);
    }
}

// Runs one queued entry. "vm" must not be one of the scheduled threads.
void dispatch(lua_State* vm, Scheduler& scheduler, const Entry& entry) {
    if (!entry.thread) {
        return; // cancelled
    }

    // Skip threads someone else already finished, closed or resumed since they were queued.
    if (lua_costatus(vm, entry.thread) == LUA_COSUS) {
        int nargs = entry.nargs;

        if (entry.isWait && lua_checkstack(entry.thread, 1)) {
            lua_pushnumber(entry.thread, scheduler.now() - entry.queuedAt);
            nargs = 1;
        }

        resumeThread(scheduler, entry.thread, nullptr, nargs);
    }

    // Only now: until here the reference kept the thread alive while it ran.
    lua_unref(vm, entry.ref);
}

void pruneDeferred(Scheduler& scheduler) {
    while (!scheduler.deferred.empty() && !scheduler.deferred.front().thread) {
        scheduler.deferred.pop_front();
    }
}

// Runs the deferred threads that are queued right now; ones queued while it runs wait for the next phase.
void runDeferred(lua_State* vm, Scheduler& scheduler) {
    for (std::size_t remaining = scheduler.deferred.size(); remaining > 0; --remaining) {
        const Entry entry = scheduler.deferred.front();
        scheduler.deferred.pop_front();
        dispatch(vm, scheduler, entry);
    }
}

// argument handling /////////////////////////////////////////

struct Prepared {
    lua_State* thread;
    int nargs;
};

// Turns "functionOrThread, ...args" (stack slots "first"..top) into a thread that is
// ready to be resumed with "nargs" values on its own stack. The thread is left on top
// of L's stack. Raises argument errors before anything else happens.
Prepared prepareThread(lua_State* L, int first) {
    luaL_argcheck(L, lua_isfunction(L, first) || lua_isthread(L, first), first, "function or thread expected");

    const int last = lua_gettop(L);
    const int count = last - first; // arguments after the function/thread
    luaL_checkstack(L, count + 2, "too many arguments");

    if (lua_isthread(L, first)) {
        lua_State* thread = lua_tothread(L, first);
        luaL_argcheck(L, lua_costatus(L, thread) == LUA_COSUS, first, "cannot schedule a thread that is not suspended");
        luaL_argcheck(L, lua_checkstack(thread, count), first, "too many arguments");

        for (int i = first + 1; i <= last; ++i) {
            lua_pushvalue(L, i);
        }
        lua_xmove(L, thread, count);
        lua_pushvalue(L, first);
        return {thread, count};
    }

    lua_State* thread = lua_newthread(L); // pushed onto L
    luaL_argcheck(L, lua_checkstack(thread, count + 1), first, "too many arguments");

    for (int i = first; i <= last; ++i) {
        lua_pushvalue(L, i);
    }
    lua_xmove(L, thread, count + 1); // the function, then its arguments
    return {thread, count};
}

// Reads a duration argument. Negative values count as 0; NaN is rejected.
double checkDuration(lua_State* L, int index) {
    const double seconds = luaL_optnumber(L, index, 0.0);
    luaL_argcheck(L, !std::isnan(seconds), index, "duration must not be NaN");
    return seconds < 0.0 ? 0.0 : seconds;
}

// Pins the thread on top of L's stack and records it as a queue entry.
Entry makeEntry(lua_State* L, Scheduler& scheduler, const Prepared& prepared) {
    Entry entry;
    entry.thread = prepared.thread;
    entry.ref = lua_ref(L, -1);
    entry.nargs = prepared.nargs;
    entry.seq = scheduler.nextSeq++;
    return entry;
}

// bindings ///////////////////////////////////////////////////////

// task.spawn(functionOrThread: (...any) -> ...any | thread, ...any): thread
int taskSpawn(lua_State* L) {
    const Prepared prepared = prepareThread(L, 1);
    resumeThread(schedulerOf(L), prepared.thread, L, prepared.nargs);
    return 1; // the thread is still on top
}

// task.defer(functionOrThread: (...any) -> ...any | thread, ...any): thread
int taskDefer(lua_State* L) {
    const Prepared prepared = prepareThread(L, 1);
    Scheduler& scheduler = schedulerOf(L);

    scheduler.deferred.push_back(makeEntry(L, scheduler, prepared));
    return 1;
}

// task.delay(duration: number?, functionOrThread: (...any) -> ...any | thread, ...any): thread
int taskDelay(lua_State* L) {
    const double seconds = checkDuration(L, 1);
    const Prepared prepared = prepareThread(L, 2);
    Scheduler& scheduler = schedulerOf(L);

    scheduler.timers.emplace(scheduler.now() + seconds, makeEntry(L, scheduler, prepared));
    return 1;
}

// task.wait(duration: number?): number
int taskWait(lua_State* L) {
    const double seconds = checkDuration(L, 1);

    if (!lua_isyieldable(L)) {
        luaL_error(L, "task.wait must be called from a coroutine that can yield");
    }

    Scheduler& scheduler = schedulerOf(L);

    Entry entry;
    entry.thread = L;
    entry.isWait = true;
    entry.queuedAt = scheduler.now();
    entry.seq = scheduler.nextSeq++;

    lua_pushthread(L);
    entry.ref = lua_ref(L, -1);
    lua_pop(L, 1);

    scheduler.timers.emplace(entry.queuedAt + seconds, entry);

    // The scheduler resumes this thread with the elapsed time, which becomes our return value.
    return lua_yield(L, 0);
}

// task.cancel(thread: thread)
int taskCancel(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State* thread = lua_tothread(L, 1);

    const int status = lua_costatus(L, thread);
    luaL_argcheck(L, status != LUA_CORUN && status != LUA_CONOR, 1, "cannot cancel a thread that is running");

    lua_State* vm = lua_mainthread(L);
    Scheduler& scheduler = schedulerOf(vm);

    for (Entry& entry : scheduler.deferred) {
        if (entry.thread == thread) {
            lua_unref(vm, entry.ref);
            entry.thread = nullptr;
            entry.ref = LUA_NOREF;
        }
    }

    for (auto it = scheduler.timers.begin(); it != scheduler.timers.end();) {
        if (it->second.thread == thread) {
            lua_unref(vm, it->second.ref);
            it = scheduler.timers.erase(it);
        } else {
            ++it;
        }
    }

    // Closing the thread makes it permanently dead, whether or not the scheduler knew it.
    lua_resetthread(thread);
    return 0;
}

constexpr NativeFunction kFunctions[] = {
    {"spawn", taskSpawn},
    {"defer", taskDefer},
    {"delay", taskDelay},
    {"wait", taskWait},
    {"cancel", taskCancel},
};

} // namespace

void openTask(lua_State* L) {
    schedulerOf(L);

    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)));
    setFunctions(L, kFunctions);
}

// host API implementation /////////////////////////////////////////////////////////

namespace task {

void setErrorHandler(lua_State* L, ErrorHandler handler) {
    schedulerOf(lua_mainthread(L)).onError = std::move(handler);
}

std::optional<double> nextWakeup(lua_State* L) {
    Scheduler* scheduler = findScheduler(lua_mainthread(L));
    if (!scheduler) {
        return std::nullopt;
    }

    pruneDeferred(*scheduler);
    if (!scheduler->deferred.empty()) {
        return 0.0;
    }

    if (scheduler->timers.empty() || scheduler->timers.begin()->first == kNever) {
        return std::nullopt;
    }

    const double wait = scheduler->timers.begin()->first - scheduler->now();
    return wait > 0.0 ? wait : 0.0;
}

bool step(lua_State* L) {
    lua_State* vm = lua_mainthread(L);
    Scheduler* scheduler = findScheduler(vm);
    if (!scheduler) {
        return false;
    }

    const double now = scheduler->now();
    const std::uint64_t cutoff = scheduler->nextSeq;

    // Anything deferred since the last cycle (by the main script, say) belongs to that cycle's end.
    runDeferred(vm, *scheduler);

    // Timers that are due. The queue is re-read on every iteration because the threads
    // we resume may schedule or cancel anything. Entries created during this step
    // (seq >= cutoff) wait for the next one, so task.wait() in a loop cannot spin here.
    while (!scheduler->timers.empty()) {
        const auto first = scheduler->timers.begin();
        if (first->first > now || first->second.seq >= cutoff) {
            break;
        }

        const Entry entry = first->second;
        scheduler->timers.erase(first);
        dispatch(vm, *scheduler, entry);
    }

    // Deferred threads queued by the timers above run at the end of this cycle.
    runDeferred(vm, *scheduler);

    return nextWakeup(vm).has_value();
}

void run(lua_State* L) {
    for (;;) {
        const std::optional<double> wait = nextWakeup(L);
        if (!wait) {
            return;
        }

        if (*wait > 0.0) {
            const double chunk = *wait < kMaxSleepSeconds ? *wait : kMaxSleepSeconds;
            std::this_thread::sleep_for(std::chrono::duration<double>(chunk));
        }

        step(L);
    }
}

} // namespace task

} // namespace sonata::lib::libs
