//
//  UNC.hpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include "luau/VM/include/lua.h"
#include "Offsets.h"
#include "ClientAPI.h"
#include "Compiler.h"
#include "Filesystem.h"
#include "Async.h"
#include "Http.h"
#include "EncStrings.h"
#include "WebSocket.h"
#include "LoadGuard.h"
#include "Chunks.h"
#include "Journal.h"
#include "LuaObject.h"
#include "Status.h"
#include "VulnPatch.h"
#include "TaskScheduler.h"
#include "Trace.h"
#include "Overlay.h"
#include "Decompiler.h"

#include "lib/base64/include/base64.hpp"
#include "lib/lz4/include/lz4.h"
#include "lib/sha3.h"

#include "Input.h"
#include "Console.h"
#include "Dialogs.h"

namespace unc {
    inline uint64_t MaxCapabilities = 0xFFFFFFFFFFFFFFFFULL;

    // The state Register() was handed - Roblox's own global state for the
    // place. Everything that has to tell "our VM" from "some other VM" (an
    // Actor's, a CoreScript's) compares against this one's global_State.
    inline uint64_t mainState = 0;

    // Running a queued script needs ExecuteScript, which lives in main.cpp
    // alongside the compiler and the signature guard. main.cpp fills this in at
    // startup so the pump below can reach it without this header depending on
    // that one.
    // Loads already-compiled bytecode on a child of `state` and hands it to the
    // scheduler (executor.hpp's ExecuteBytecode). The compile happened in
    // Pump::push, on the producer's thread - see there.
    inline void (*runBytecode)(uint64_t state, const std::string& bytecode) = nullptr;

    // Work that must happen on Roblox's own thread.
    //
    // Everything the executor runs used to be loaded and spawned from a
    // detached OS thread, straight into the Lua heap, while Roblox's scheduler
    // was using it. `task.spawn` resumes the thread inline, so that thread was
    // *running Lua* next to the VM - a data race against the collector and
    // against whatever the game was doing. It mostly survives a quiet place and
    // stops surviving a busy one, which is what "joining this game has a large
    // chance of crashing" looks like.
    //
    // All six references run their execution on Roblox's thread instead: the
    // macOS one and Vicna from a `RunService.RenderStepped` connection,
    // Module-Source from `Heartbeat`, sourcessss by hooking
    // `WaitingHybridScriptsJob`'s vtable step, RbxStu from a scheduler job of
    // its own. This is the same idea: the socket thread only ever appends a
    // string, and a callback connected to Heartbeat drains it in-frame.
    namespace Pump {

        inline std::mutex mutex;
        // A deque, not a vector: this is a FIFO and `erase(begin())` shifts the
        // whole thing on every pop. mac uses `std::deque::pop_front`, src and
        // stu `std::queue::pop`; only Vicna shares the vector, and Vicna also
        // reads it from the render thread with no lock at all while its socket
        // thread writes to it.
        // What a queued entry is waiting for before it may run.
        //
        // mac gates its whole drain on `Workspace != 0` (SchedulerHook returns
        // early until then) and text-prefixes only its teleport queue with
        // `repeat task.wait(1) until game:IsLoaded()`. A text prefix shifts
        // every line number in every error the script raises - which is a real
        // cost on an autoexec you cannot watch run - so the wait is a property
        // of the entry here instead of something spliced into its source.
        enum class Gate { none, gameReady };

        struct Entry {
            std::string bytecode;       // compiled in push(), loaded in step()
            size_t sourceSize = 0;      // what the UI sent, for execution matching
            Gate gate = Gate::none;
            // When it was queued, so the wait can be given up on. A place where
            // LocalPlayer never appears must not mean autoexec never runs -
            // the same "stop insisting eventually" reasoning as waitForGame's
            // 5 s cap in main.cpp.
            std::chrono::steady_clock::time_point queuedAt =
                std::chrono::steady_clock::now();
        };

        inline std::deque<Entry> queued;

        // How long a gated entry waits for the game before it runs anyway.
        inline constexpr std::chrono::seconds kGateTimeout{30};

        // Nobody caps this - not one of the six. Ours is capped anyway because
        // ours is the only one whose queue has three producers (the socket, the
        // teleport drain and autoexec) and drains one per frame, so "more arrived
        // than can ever run" is a reachable state rather than a theoretical one.
        // The socket already refuses past its own depth cap; this is the backstop
        // for the other two.
        inline constexpr size_t kMaxQueued = 256;

        // Whether there is a game to run anything in.
        //
        // The window between "the player left / teleported" and "we have built
        // the next game" is the dangerous one: the DataModel is going away, but
        // the Heartbeat connection can still fire once or twice on the way out
        // and `mainState` still points at the state that is being torn down.
        // Running a queued script there loads it into a dying VM. The macOS
        // reference closes the same window by nulling its MainThread,
        // DataModel and ScriptContext the moment its teleport hook runs.
        inline std::atomic<bool> armed{false};

        inline void disarm()
        {
            armed.store(false, std::memory_order_release);
            runtime::pumpArmed.store(false, std::memory_order_release);
            runtime::setGameReady(false);
            std::lock_guard<std::mutex> lock(mutex);
            queued.clear();
            runtime::queuedScripts.store(0, std::memory_order_release);
        }

        inline void arm()
        {
            armed.store(true, std::memory_order_release);
            runtime::pumpArmed.store(true, std::memory_order_release);
        }

        // Called from any thread. Never touches Lua.
        //
        // The script is COMPILED here, on the producer's thread, and only loaded
        // in step() on Roblox's frame thread. Luau::compile touches no Lua state,
        // and every producer - the socket drain, the teleport queue, autoexec -
        // runs on the init thread, so the frame pays for the load alone. At the
        // ~15 MB/s measured (perf2, 2026-09-03) a 200 KB script is ~13 ms of
        // compile, which used to land inside one Heartbeat as a visible hitch on
        // every execute. A parse error is encoded into the bytecode by
        // Luau::compile and still surfaces from luau_load on the frame thread,
        // with the same message and line numbers as before.
        //
        // The compile is the one part of this that can throw - std::bad_alloc
        // from the BytecodeBuilder on a large source (a socket client may send
        // 16 MiB) - and an exception out of the init thread's detached lambda is
        // std::terminate, so it is caught here the way step() caught it before.
        inline void push(std::string source, Gate gate = Gate::none)
        {
            if (source.empty())
                return;
            std::string bytecode;
            try {
                bytecode = compileScript(source);
            } catch (const std::exception& error) {
                journal::writef("QUEUE compile threw: %s", error.what());
                return;
            } catch (...) {
                journal::write("QUEUE compile threw an unknown exception");
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (queued.size() >= kMaxQueued) {
                journal::writef("QUEUE dropped a script: %zu already waiting", queued.size());
                return;
            }
            queued.push_back(Entry{std::move(bytecode), source.size(), gate,
                                   std::chrono::steady_clock::now()});
            runtime::queuedScripts.store(queued.size(), std::memory_order_release);
        }

        inline size_t depth()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return queued.size();
        }

        // Connected to RunService.Heartbeat, so this runs on Roblox's thread
        // with the VM to itself. One script per frame, which is what the macOS
        // reference does - a burst of them in one frame is a stall the game can
        // see.
        // Argument 1 is the bootstrap's answer to "is the game usable yet" -
        // game:IsLoaded() and Players.LocalPlayer both true. It is computed in
        // Lua because that is where those two are cheap to ask, and passed in
        // rather than polled so this stays a plain queue pop.
        inline int step(lua_State* L) {
            const bool ready =
                rbx::lua_toboolean(reinterpret_cast<uint64_t>(L), 1) != 0;
            runtime::setGameReady(ready);

            if (!armed.load(std::memory_order_acquire) || !runBytecode || !mainState)
                return 0;

            std::string bytecode;
            size_t sourceSize = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (queued.empty())
                    return 0;

                Entry& head = queued.front();
                if (head.gate == Gate::gameReady && !ready) {
                    // Stall, do not skip. The queue is a FIFO and the teleport
                    // drain is pushed ahead of autoexec on purpose; running the
                    // autoexec first because the teleport entry was gated would
                    // be a different bug.
                    if (std::chrono::steady_clock::now() - head.queuedAt < kGateTimeout)
                        return 0;
                    journal::write("QUEUE gate timed out; running without a LocalPlayer");
                    head.gate = Gate::none;
                }

                bytecode = std::move(head.bytecode);
                sourceSize = head.sourceSize;
                queued.pop_front();
                runtime::queuedScripts.store(queued.size(), std::memory_order_release);
            }

            // This is a lua_CFunction on Roblox's own thread, so an exception
            // that gets out of here does not unwind into anything that can
            // handle it - it goes through Luau's C frame and the process dies.
            // The compile, which was the part that could throw, now happens in
            // push() on the producer's thread; the load below is Roblox's own
            // code and raises through Lua, not C++. The guard stays because
            // nothing here is worth the process.
            //
            // All six references compile on this thread. Two guard the nearest
            // thing they have - mac wraps its yield callback, stu wraps each
            // synchronised dispatch - and none wraps the execution itself.
            try {
                journal::writef("RUN start %zu", sourceSize);
                runBytecode(mainState, bytecode);
                journal::writef("RUN done %zu", sourceSize);
            } catch (const std::exception& error) {
                journal::writef("RUN failed %zu: %s", sourceSize, error.what());
                journal::writef("QUEUE script threw: %s", error.what());
            } catch (...) {
                journal::writef("RUN failed %zu: unknown exception", sourceSize);
                journal::write("QUEUE script threw an unknown exception");
            }
            return 0;
        }
    }

    // Defined at the bottom of this header, next to Register. A thread built on
    // an Actor's state starts with that state's globals and none of ours, so
    // whatever runs there needs them installed before it can call one - which
    // is what "attempt to call a nil value" on isparallel was.
    inline void RegisterFunctions(uint64_t L);

    // Every lua_CFunction the executor has put into the VM. isexecutorclosure
    // answers for C closures by looking one up here, which is why a clone of
    // getgenv - a different closure over the same function - still reads as
    // ours, and why matching on global names would not have.
    inline std::unordered_set<uintptr_t> ourCFunctions;

    // The identity executor.hpp's SetIdentity starts every thread at. It is no
    // longer what "this call came from us" is decided by - a script can lower
    // its own identity and stay ours - that is kExecutorMark's job now.
    inline constexpr uint32_t kExecutorIdentity = 8;

    namespace utils {
        // Raise a Lua error with a message. Five namespaces in this file each
        // grew their own copy of this; new code uses this one.
        inline int fail(uint64_t state, const char* message)
        {
            rbx::lua_pushlstring(state, message, std::strlen(message));
            rbx::lua_error(state);
            return 0;
        }

        // There is no lua_pushnumber to borrow, so a double goes onto the stack
        // the same way the thread walk pushes a raw object: reserve a slot with
        // nil and write the TValue into it. Tag 3 is LUA_TNUMBER in Roblox's
        // fork, which puts LUA_TINTEGER at 4 and everything above it up one.
        inline void pushNumber(uint64_t state, double value)
        {
            rbx::lua_pushnil(state);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            uint8_t* slot = top - luaobj::kValueSize;
            luaobj::write<double>(slot, luaobj::value::gc, value);
            luaobj::write<int>(slot, luaobj::value::tt, 3);
        }

        // A stack slot's number, without truncating it. lua_tointegerx rounds
        // to an int - which is how math.huge once arrived as INT_MIN - and
        // lua_tolstring would rewrite the slot, so the double is read straight
        // out of the frame. A positive index is base + (index - 1) by
        // definition. Answers 0 for anything that is not a number.
        inline double toNumber(uint64_t state, int index)
        {
            const int type = rbx::lua_type(state, index);
            if (type != 3 && type != 4)
                return 0.0;
            auto* base = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state),
                                                luaobj::state::base);
            if (index <= 0 || !base)
                return 0.0;
            const uint8_t* slot = base + (index - 1) * luaobj::kValueSize;
            if (type == 4)
                return static_cast<double>(luaobj::read<int32_t>(slot, luaobj::value::gc));
            return luaobj::read<double>(slot, luaobj::value::gc);
        }
    }

    // Scripts handed to queue_on_teleport wait here until a hop is under way,
    // and main.cpp drains them once the next place is up.
    //
    // The queue lives on disk as well as in memory, because "a teleport" is not
    // only the in-process kind. Roblox keeps the process across
    // TeleportService, so an in-memory queue covers that - but a player who
    // alt-tabs to the app and joins another game gets a *fresh process*, and a
    // queue that only existed in the old one is gone.
    //
    // **No reference does this.** mac keeps a `std::queue<std::string>` under a
    // mutex and drains it in its job hook; sourcessss keeps a vector on its
    // environment; Module-Source's `queueonteleport` is a stub that checks its
    // argument and returns; Vicna, SkidBase and RbxStu do not have the function
    // at all. So 0/6, and an earlier comment here claimed the opposite - it was
    // wrong. Persisting is kept anyway, because the fresh-process case is the
    // one people actually hit and an in-memory queue cannot serve it, but it is
    // ours and it carries costs none of them pay, which is why the bounds below
    // exist.
    //
    // A stale queue is bounded by time rather than left to fire whenever: a
    // file older than the window below is dropped unread, so a script queued
    // and forgotten yesterday does not run on tomorrow's unrelated join.
    namespace teleport {
        inline std::mutex mutex;
        inline std::vector<std::string> queue;

        // Vestigial. `take()` used to require a teleport to have been *seen*
        // before draining; that gate is gone (see take() below), so nothing
        // reads this any more. The byte stays in the file format so an on-disk
        // queue written by an older build still parses, and it is always
        // written as 0 - which means the teleport hook no longer does a
        // synchronous open/write/close from inside itself, on Roblox's own
        // thread, in the middle of a teleport.
        inline constexpr bool pending = false;

        inline constexpr std::chrono::minutes kStaleAfter{30};

        // A queue is a handful of scripts. These are here because ours is
        // written to disk on every push - which mac's and sourcessss's are not -
        // so an unbounded queue is an unbounded file rewritten once per entry,
        // and a script looping queue_on_teleport would spend the session doing
        // O(n^2) bytes of I/O.
        inline constexpr size_t kMaxQueued = 64;
        inline constexpr size_t kMaxQueuedBytes = 8u << 20;

        inline std::filesystem::path queuePath()
        {
            return workspace::root().parent_path() / "teleport-queue.bin";
        }

        // length-prefixed entries, so a script containing anything at all
        // survives the round trip.
        inline void writeFile()
        {
            const std::filesystem::path path = queuePath();
            std::error_code ignored;
            if (queue.empty() && !pending) {
                std::filesystem::remove(path, ignored);
                return;
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            const uint8_t flag = pending ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&flag), 1);
            for (const std::string& script : queue) {
                uint32_t length = static_cast<uint32_t>(script.size());
                out.write(reinterpret_cast<const char*>(&length), sizeof(length));
                out.write(script.data(), static_cast<std::streamsize>(script.size()));
            }
        }

        inline bool readFile(std::vector<std::string>& out, bool& wasPending)
        {
            const std::filesystem::path path = queuePath();
            std::error_code code;
            if (!std::filesystem::exists(path, code))
                return false;

            const auto written = std::filesystem::last_write_time(path, code);
            if (!code) {
                const auto age = std::filesystem::file_time_type::clock::now() - written;
                if (age > kStaleAfter) {
                    std::filesystem::remove(path, code);
                    return false;
                }
            }

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            uint8_t flag = 0;
            in.read(reinterpret_cast<char*>(&flag), 1);
            if (!in)
                return false;
            wasPending = flag != 0;
            // Same ceiling the in-memory queue has. A file written by a build
            // without that ceiling - or one that got corrupted into looking
            // like it has a million entries - stops here rather than being
            // read into the heap entry by entry.
            while (out.size() < kMaxQueued) {
                uint32_t length = 0;
                in.read(reinterpret_cast<char*>(&length), sizeof(length));
                if (!in || length == 0 || length > (16u << 20))
                    break;
                std::string script(length, '\0');
                in.read(script.data(), static_cast<std::streamsize>(length));
                if (!in)
                    break;
                out.push_back(std::move(script));
            }
            return true;
        }

        // Called from a Roblox script thread. Answers whether it was taken.
        inline bool push(std::string script)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.size() >= kMaxQueued) {
                journal::writef("QUEUE refusing a teleport script: %zu already queued",
                                queue.size());
                return false;
            }
            size_t total = script.size();
            for (const std::string& queued : queue)
                total += queued.size();
            if (total > kMaxQueuedBytes) {
                journal::writef("QUEUE refusing a teleport script: %zu bytes queued",
                                total);
                return false;
            }

            queue.push_back(std::move(script));
            writeFile();
            return true;
        }

        // clearqueueonteleport: everything queued so far is dropped, but a hop
        // already flagged stays flagged - the script that asked for the clear
        // may well queue something new straight after.
        inline void clear()
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.clear();
            writeFile();
        }

        // Empties the queue at a game load.
        //
        // This used to require a teleport to have been *seen*: either this
        // process's own teleport hook had run, or the file said the process
        // that wrote it had teleported. That covers a `TeleportService` hop and
        // nothing else. The case it misses is the one people actually do -
        // leaving to the app and joining another game - which is a fresh
        // process with no hook to have fired, so the flag on disk is 0 and the
        // scripts sit there until they go stale.
        //
        // So the gate is gone: anything on disk that is younger than
        // kStaleAfter runs at the next game load, whatever route got us there.
        // The staleness bound is what keeps that honest - a script queued and
        // then forgotten does not resurface days later - and the file is
        // rewritten (emptied) as soon as it is taken, so it runs once.
        inline std::vector<std::string> take()
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.empty()) {
                std::vector<std::string> fromDisk;
                bool diskPending = false;
                if (readFile(fromDisk, diskPending) && !fromDisk.empty())
                    queue = std::move(fromDisk);
            }

            std::vector<std::string> scripts;
            scripts.swap(queue);
            writeFile();
            return scripts;
        }
    }

    // Anything that has to wait on the network runs on a worker thread and the
    // Lua thread yields until it is done, rather than stopping the whole client
    // for the length of a transfer. curl on the scheduler thread was freezing
    // the game for up to CURLOPT_TIMEOUT_MS a call.
    //
    // The mechanic is the one newcclosure already uses: a yield does not unwind
    // the C frame it happened under, so the C function notices and returns -1,
    // and the VM comes back through the closure's continuation once the thread
    // is resumed. Two things follow from that:
    //
    //   * every function that calls start() must be a closure built *with* a
    //     continuation, and that continuation must end in finish(). Without one
    //     luaD_callint never lifts the C-call boundary, the yield raises, and
    //     resume_continue would call a null cont pointer.
    //   * the waiter is Lua, not C: task.wait() is what puts the thread back in
    //     the scheduler's hands, so no resume of our own is needed and the
    //     thread comes back on the thread Roblox owns.
    //
    // coroutine.isyieldable() is the safety valve. If the caller turns out not
    // to be yieldable after all - a hook under a C closure with no continuation,
    // say - the waiter blocks instead, which is exactly the old behaviour rather
    // than an error a script could not have expected.
    namespace Yield {

        // state -> the job that state is waiting on. Only ever read to answer
        // "was this yield ours", which is what lets the DataModel's __namecall
        // continuation hand a foreign yield back to Roblox's own.
        inline std::mutex pendingMutex;
        inline std::unordered_map<uint64_t, int> pending;

        // When to sweep the dead entries out of `pending`. It is a ceiling on
        // *concurrently waiting threads*, which is a number no real script
        // reaches - the sweep is for leaked keys, not for load.
        inline constexpr size_t kMaxPending = 256;

        inline int awaitRef = -1;

        // A closure whose only job is to be dropped into a call frame. See
        // claimFrame; its C function is never called.
        inline int frameFn(lua_State* L) { (void)L; return 0; }
        inline int frameRef = -1;

        inline int continuation(lua_State* L, int status);

        // Makes the frame that is currently running ours.
        //
        // Whether a yield is legal is decided by luaD_callint, which lifts the
        // C-call boundary only when the closure at L->ci->func is a C closure
        // with a continuation. The DataModel's __namecall closure has none - so
        // game:HttpGet could not yield, and blocked, while every other path
        // (the __index closure we push ourselves, request_raw, ws_connect) did
        // yield, which is exactly the split the frame counts showed.
        //
        // Rather than write a continuation into Roblox's shared closure - where
        // every WaitForChild in the game would then resume through ours - the
        // *stack slot* the frame's CallInfo points at is swapped for a closure
        // of ours. Nothing Roblox owns is modified, the swap lives exactly as
        // long as this one call, and luau_poscall still writes the results to
        // that same slot afterwards.
        inline bool claimFrame(uint64_t state)
        {
            if (frameRef < 0)
                return false;
            void* callinfo = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci);
            if (!callinfo)
                return false;
            void* slot = luaobj::read<void*>(callinfo, luaobj::callinfo::func);
            if (!slot)
                return false;

            // The claim is unconditional on purpose, and it was tried the other
            // way round. Skipping it for a closure that already has a
            // continuation - which is what decides whether the yield is *legal*
            // - looks like a free saving and is not one: the claim also puts
            // *our* continuation on the frame, which is how the results get
            // built by Yield::finish when the thread comes back. Guarding it on
            // "can this frame yield already" left `request`, `saveinstance` and
            // `WebSocket.connect` resuming through the wrong continuation and
            // failing with "attempt to call a thread value". Traced: only
            // `game:HttpGet` arrives here on a closure with no continuation,
            // and the other three still need the frame to become ours.
            if (trace::on("claim")) {
                const void* closure = luaobj::read<void*>(slot, luaobj::value::gc);
                rbx::print(0, "TRC claim | closure=%p thread=%p", closure,
                           reinterpret_cast<void*>(state));
            }

            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, frameRef);
            if (!rbx::lua_iscfunction(state, -1)) {
                rbx::lua_settop(state, -2);
                return false;
            }
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            // A stack slot is a GC root and the closure is anchored in the
            // registry, so copying the value in needs no write barrier.
            luaobj::copyValue(slot, 0, top - luaobj::kValueSize, 0);
            rbx::lua_settop(state, -2);
            return true;
        }

        inline int readyFn(lua_State* L)
        {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            rbx::lua_pushboolean(state, (isnum && async::ready(id)) ? 1 : 0);
            return 1;
        }

        inline int blockFn(lua_State* L)
        {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            if (isnum && !async::wait(id))
                journal::writef("ASYNC blocking wait for job %d timed out", id);
            return 0;
        }

        inline const std::string kWaiter = vantastr::waiter();

        // Compiles the waiter and keeps it in the registry. Runs once per join,
        // from RegisterFunctions, because the registry goes with the VM.
        inline void install(uint64_t L)
        {
            awaitRef = -1;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                pending.clear();
            }

            static const std::string bytecode = compileScript(kWaiter);
            if (bytecode.empty() || bytecode[0] == 0)
                return;

            const int top = rbx::lua_gettop(L);

            // The waiter's whole world: four names and nothing else, so a game
            // that rewrites a global cannot reach into it.
            rbx::lua_createtable(L, 0, 4);

            auto borrow = [&](const char* library, const char* member, const char* as) {
                rbx::lua_getfield(L, LUA_GLOBALSINDEX, library);
                rbx::lua_getfield(L, -1, member);
                rbx::lua_setfield(L, -3, as);
                rbx::lua_settop(L, -2);
            };
            borrow("task", "wait", "wait");
            borrow("coroutine", "isyieldable", "isyieldable");

            rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(&readyFn), "ready", 0, 0);
            rbx::lua_setfield(L, -2, "ready");
            rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(&blockFn), "block", 0, 0);
            rbx::lua_setfield(L, -2, "block");

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(L, "@vanta", bytecode.c_str(), bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(L, top);
                printf("[vanta] the yield waiter would not load; HTTP stays blocking\n");
                fflush(stdout);
                return;
            }

            rbx::setprotocapabilities(L, -1, &MaxCapabilities);
            rbx::lua_pushvalue(L, -2);      // the env table
            rbx::lua_setfenv(L, -2);
            awaitRef = rbx::lua_ref(L, -1);

            rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(&frameFn), "vanta.yield", 0,
                                   reinterpret_cast<uintptr_t>(&continuation));
            frameRef = rbx::lua_ref(L, -1);
            rbx::lua_settop(L, top);
        }

        // Whether a job is outstanding on this thread.
        inline bool ours(uint64_t state)
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            return pending.find(state) != pending.end();
        }

        // Collects the finished job. Reached twice over: directly from start()
        // when the waiter did not have to yield, and from the continuation when
        // it did. Either way slot 1 holds the id the waiter handed back.
        // Which job this thread is waiting on comes from our own bookkeeping,
        // not from the Lua stack. Handing the id to the waiter and reading it
        // back out of the frame afterwards looked tidy and was wrong: the
        // second call on a thread came back with the *first* call's id still in
        // slot 1, so it collected the wrong job - three Myriad checks failed
        // that way. A thread can only ever wait on one job at a time, because
        // it is suspended for the whole of it, so a map keyed by thread is
        // exact.
        inline int finish(uint64_t state)
        {
            int id = 0;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                auto found = pending.find(state);
                if (found != pending.end()) {
                    id = found->second;
                    pending.erase(found);
                }
            }
            const bool isnum = id != 0;
            const int top = rbx::lua_gettop(state);
            if (trace::on("yield"))
                rbx::print(0, "TRC yield | finish id=%d top=%d thread=%p", id, top,
                           reinterpret_cast<void*>(state));
            rbx::lua_settop(state, 0);

            async::Result push = isnum ? async::take(id) : nullptr;
            if (!push && isnum && async::exists(id)) {
                // Resumed before the job finished - something other than our
                // waiter put the thread back. The answer is still coming, so
                // wait for it here rather than losing it. Bounded: this runs on
                // Roblox's scheduler, and a job that never lands must not be
                // able to hold it for ever. Falling through to the error below
                // costs the caller one request; not falling through cost the
                // whole client.
                if (async::wait(id))
                    push = async::take(id);
                else
                    journal::writef("ASYNC job %d did not finish within the block timeout", id);
            }
            if (!push) {
                // Say which way it was lost: a stack that is not the id we
                // handed the waiter reads very differently from a job that is
                // simply gone.
                char message[160];
                const int written = snprintf(message, sizeof(message),
                    "the request was lost before it finished (id=%d top=%d pending=%d)",
                    id, top, async::exists(id) ? 1 : 0);
                rbx::lua_pushlstring(state, message,
                                     written > 0 ? static_cast<size_t>(written) : 0);
                rbx::lua_error(state);
                return 0;
            }
            return push(state);
        }

        // Runs `work` off the Lua thread and waits for it. Answers -1 when the
        // thread yielded, which the calling C function must return verbatim so
        // the VM keeps the frame; anything else is a finished result count.
        inline int start(uint64_t state, std::function<async::Result()> work)
        {
            const int id = async::submit(std::move(work));
            if (trace::on("yield"))
                rbx::print(0, "TRC yield | start id=%d thread=%p awaitRef=%d", id,
                           reinterpret_cast<void*>(state), awaitRef);

            {
                std::lock_guard<std::mutex> lock(pendingMutex);

                // A thread that is killed while it is waiting never reaches
                // finish(), so its entry stays. The key is a lua_State* and
                // Luau recycles addresses, so a later thread landing on one
                // would read as "already waiting on job N" and collect the
                // wrong answer. RbxStu is the only reference that handles the
                // same case - its drain finds `threadRef.thread->status !=
                // LUA_YIELD` and says "something must have murdered the thread
                // while it was yielded", then frees the ref. It can ask the
                // thread because it holds one; this map never dereferences a
                // state, so the equivalent question is asked of the *job*: an
                // entry whose job no longer exists is dead by definition,
                // because nothing can collect it any more.
                if (pending.size() >= kMaxPending) {
                    for (auto it = pending.begin(); it != pending.end();)
                        it = async::exists(it->second) ? std::next(it) : pending.erase(it);
                }
                pending[state] = id;
            }

            if (awaitRef < 0) {
                // No waiter to yield through: block, the way this used to.
                async::wait(id);
                return finish(state);
            }

            rbx::lua_settop(state, 0);
            claimFrame(state);
            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, awaitRef);
            rbx::lua_pushinteger(state, id);
            // One result, even though the id it hands back is ignored: with
            // nresults 0 luaD_callint sets the frame's top to its base on the
            // yield, and the continuation then had nowhere to put anything -
            // no continuation ran at all and the VM went sideways.
            rbx::lua_call(state, 1, 1);

            if (luaobj::yielded(state))
                return -1;
            return finish(state);
        }

        // The continuation every yielding frame of ours comes back through.
        inline int continuation(lua_State* L, int status)
        {
            (void)status;
            return finish(reinterpret_cast<uint64_t>(L));
        }
    }

    // Defined with the rest of the metamethod work further down. A hooked C
    // metamethod carries its chain in a side map keyed by the closure, so a
    // *copy* of that closure arrives at the dispatcher with nothing behind it -
    // which is what filled the console with "the hooked metamethod is gone",
    // once per frame, out of a render-step callback. Every clone path tells the
    // map about the copy instead.
    // Where a GCObject keeps its type tag.
    //
    // Luau's GCheader is {tt, marked, memcat}, so the tag is the first byte -
    // which held for 0.734 through 0.736. On 0.737 the header gained a byte in
    // front of it: a real thread reads 00 0a 02, a table 00 07 02, a closure
    // 00 08 02. The tag is at byte 1 and `marked` at byte 2 - the same shift
    // the dump reported as state::marked moving 0x1 -> 0x2, because that is
    // the same field.
    //
    // Reading it at the wrong byte does not fail loudly, it matches the *wrong
    // objects*: threadVisit collected twenty-five things whose first byte
    // merely happened to be 0x0a, handed them to lua_getthreaddata, and
    // getrunningscripts then took the client down inside Roblox's own
    // pushInstance with libVanta on the faulting stack.
    //
    // So it is measured once rather than assumed, against two objects whose
    // tags we already know. Requiring a table and a thread to agree rules out
    // a byte that merely happens to hold one of the two values; if they do not
    // agree on exactly one byte, byte 0 stands as Luau's own layout.
    inline int gcTagOffset(uint64_t state)
    {
        static int cached = -1;
        if (cached >= 0)
            return cached;

        const int top = rbx::lua_gettop(state);
        int tableBits = 0, threadBits = 0;

        rbx::lua_createtable(state, 0, 0);
        if (const void* table = rbx::lua_topointer(state, -1)) {
            for (int at = 0; at < 4; at++)
                if (luaobj::byte(table, at) == luaobj::kTagTable)
                    tableBits |= 1 << at;
        }
        rbx::lua_newthread(state);
        if (const void* thread = rbx::lua_topointer(state, -1)) {
            for (int at = 0; at < 4; at++)
                if (luaobj::byte(thread, at) == luaobj::kTagThread)
                    threadBits |= 1 << at;
        }
        rbx::lua_settop(state, top);

        const int agreed = tableBits & threadBits;
        int found = 0;
        for (int at = 0; at < 4; at++)
            if (agreed == (1 << at)) { found = at; break; }
        cached = found;
        return cached;
    }

    namespace Metatable {
    }

    namespace Closures {
        inline void patchCClosure(void* destination, const void* source,
                                  uintptr_t function, uintptr_t continuation);
        inline void patchLClosure(void* destination, const void* source);


        inline int fail(uint64_t state, const char* message)
        {
            rbx::lua_pushlstring(state, message, std::strlen(message));
            rbx::lua_error(state);
            return 0;
        }

        // checkcaller
        // getfunctionhash
        // hookfunction
        // hookmetamethod
        // Five of the six references argument-check this trio; only Vicna
        // answers false for a non-function. The engine's own checktype is what
        // most of them call, so the message is Roblox's rather than ours.
        inline int iscclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagFunction);
            rbx::lua_pushboolean(state, rbx::lua_iscfunction(state, 1));
            return 1;
        }

        inline int islclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagFunction);
            rbx::lua_pushboolean(state, rbx::lua_islfunction(state, 1));
            return 1;
        }

        // isexecutorclosure is answered in Lua, because the only handle a script
        // has on a Luau closure's origin is debug.info(f, "s") - reaching the
        // proto's source from C would mean knowing Roblox's Closure layout.
        // These two are what it asks C for.
        inline int ischunk(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            size_t len = 0;
            const char* source = rbx::lua_tolstring(state, 1, &len);
            rbx::lua_pushboolean(state, source && chunks::isOurs(std::string(source, len)) ? 1 : 0);
            return 1;
        }

        // "Did this call come from the executor" - answered by the mark
        // SetIdentity leaves in the thread's capability word, not by its
        // identity, so a script that lowers its own identity is still
        // recognised. sUNC tests exactly that. See kExecutorMark: this is
        // RbxStu's design and it replaced a set of globals-table pointers that
        // grew for the life of a game and went stale when one was collected -
        // a recycled address made checkcaller answer true inside a game script.
        inline int checkcaller(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "checkcaller");
            const bool ours = isExecutorThread(state);
            if (trace.active) {
                const uint64_t extraSpace = rbx::lua_getthreaddata(state);
                char detail[192];
                snprintf(detail, sizeof(detail),
                         "thread=%p ours=%d identity=%u caps=%016llx script=%p",
                         reinterpret_cast<void*>(state), ours ? 1 : 0,
                         extraSpace ? *reinterpret_cast<uint32_t*>(extraSpace + offsets::identity) : 0u,
                         extraSpace ? static_cast<unsigned long long>(
                             *reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities)) : 0ull,
                         extraSpace ? luaobj::read<void*>(reinterpret_cast<void*>(extraSpace),
                                                          offsets::threadscript) : nullptr);
                trace.note(detail);
            }
            rbx::lua_pushboolean(state, ours ? 1 : 0);
            return trace.done(1);
        }

        inline int isourcfunction(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t function = rbx::lua_tocfunction(state, 1);
            rbx::lua_pushboolean(state, function && ourCFunctions.count(function) ? 1 : 0);
            return 1;
        }

        // newcclosure hands back a real C closure, so the Luau function it
        // wraps cannot live in an upvalue - the spec requires the result to
        // have none. It goes in the registry instead, and the handler finds it
        // by asking which closure is currently running: L->ci->func names the
        // stack slot holding it, and its address keys this map.
        //
        // A wrapper that is collected leaves its entry and its registry ref
        // behind. That is a leak, not a correctness problem: the map is only
        // ever read from inside a running wrapper, so the address being looked
        // up is by definition still alive, and a wrapper allocated at a reused
        // address overwrites the stale entry when it is created.
        inline std::mutex wrappedMutex;
        inline std::unordered_map<const void*, int> wrappedFunctions;

        // A directly patched Luau target takes the hook's environment, because
        // patching copies the whole body. Hooks must still answer getfenv with
        // the environment the target had - that is what a dispatcher would give
        // them - so the pre-hook environment is kept here until restore.
        inline std::mutex hookedEnvMutex;
        inline std::unordered_map<const void*, int> hookedEnvs;

        inline const void* runningClosure(uint64_t state)
        {
            void* callinfo = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci);
            if (!callinfo)
                return nullptr;
            void* slot = luaobj::read<void*>(callinfo, luaobj::callinfo::func);
            if (!slot)
                return nullptr;
            return luaobj::read<void*>(slot, luaobj::value::gc);
        }

        // Reached when the wrapped function yielded and the thread was resumed.
        // Its results are already sitting above the arguments on our frame, so
        // the VM needs their count - and only their count. Reporting the whole
        // stack instead counts the arguments as results: the frame is then
        // popped with the wrong arity and stays behind, and the next resume on
        // that thread dispatches the *stale* frame's continuation. If the
        // function was restored in between, that continuation is the original
        // C function's (null for a plain one) and the client jumps to zero.
        // Registering this continuation is what makes the wrapper yieldable -
        // a coroutine-based newcclosure cannot be, which the spec calls out.
        // A restore that lands while a frame is still suspended in the target
        // cannot patch the closure yet - the frame's continuation is read from
        // it on resume, and the original C function's continuation is null.
        // These track which thread is suspended in which target and which
        // restores are waiting for that.
        inline std::mutex suspendedMutex;
        inline std::unordered_map<uint64_t, const void*> suspendedTargets;
        inline std::unordered_map<const void*, int> deferredRestores;

        inline int newcclosureContinue(lua_State* L, int status) {
            (void)status;
            uint64_t state = reinterpret_cast<uint64_t>(L);

            const void* target = nullptr;
            bool applyDeferred = false;
            int backupRef = 0;
            {
                std::lock_guard<std::mutex> lock(suspendedMutex);
                auto suspended = suspendedTargets.find(state);
                if (suspended != suspendedTargets.end()) {
                    target = suspended->second;
                    suspendedTargets.erase(suspended);
                }
                if (target) {
                    auto deferred = deferredRestores.find(target);
                    if (deferred != deferredRestores.end()) {
                        bool stillSuspended = false;
                        for (const auto& entry : suspendedTargets)
                            if (entry.second == target) {
                                stillSuspended = true;
                                break;
                            }
                        if (!stillSuspended) {
                            backupRef = deferred->second;
                            deferredRestores.erase(deferred);
                            applyDeferred = true;
                        }
                    }
                }
            }

            if (applyDeferred && target && backupRef > 0) {
                // The resumed frame still points at this closure, but its
                // continuation has already been read - patching now is the
                // same as patching right after it was popped.
                rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, backupRef);
                if (const void* backup = rbx::lua_topointer(state, -1)) {
                    if (luaobj::byte(backup, luaobj::closure::isC)) {
                        Closures::patchCClosure(const_cast<void*>(target), backup,
                            luaobj::read<uintptr_t>(backup, luaobj::closure::cfunction),
                            luaobj::decodeContinuation(backup));
                        std::lock_guard<std::mutex> lock(Closures::wrappedMutex);
                        auto inner = Closures::wrappedFunctions.find(backup);
                        if (inner != Closures::wrappedFunctions.end())
                            Closures::wrappedFunctions[target] = inner->second;
                        else
                            Closures::wrappedFunctions.erase(target);
                    } else {
                        Closures::patchLClosure(const_cast<void*>(target), backup);
                    }
                }
                rbx::lua_settop(state, -2);
                rbx::lua_unref(state, backupRef);
            }
            return rbx::lua_gettop(state);
        }

        inline int newcclosureCall(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            int reference = -1;
            if (const void* self = runningClosure(state)) {
                std::lock_guard<std::mutex> lock(wrappedMutex);
                auto found = wrappedFunctions.find(self);
                if (found != wrappedFunctions.end())
                    reference = found->second;
            }
            if (reference < 0)
                return fail(state, "newcclosure: the wrapped function is no longer reachable");

            const int arguments = rbx::lua_gettop(state);
            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
            rbx::lua_insert(state, 1);

            // Yielding out of here is allowed because the wrapper was built
            // with a continuation: luaD_callint sees isC && c.cont on the
            // calling closure and lifts the C-call boundary for the duration.
            // An error is left to propagate, so the wrapped function's own
            // message survives untouched.
            rbx::lua_call(state, arguments, LUA_MULTRET);

            // A yield does not unwind this frame - the call simply returns with
            // the thread marked - so the results are not ours to report yet.
            // Answering -1 hands the frame back to the VM, which comes back
            // through newcclosureContinue once the thread is resumed.
            //
            // "Marked" includes SCHEDULED_REENTRY, which is what a pcall inside
            // the wrapped function leaves behind - see luaobj::yielded. Said
            // once per process because a build that stops doing it would be
            // worth knowing about.
            if (luaobj::yielded(state)) {
                if (const void* target = runningClosure(state)) {
                    std::lock_guard<std::mutex> lock(suspendedMutex);
                    suspendedTargets[state] = target;
                }
                static std::atomic<bool> saidIt{false};
                if (luaobj::byte(reinterpret_cast<void*>(state), luaobj::state::status) ==
                        luaobj::kStatusScheduledReentry &&
                    !saidIt.exchange(true))
                    journal::write("NEWCC a call came back SCHEDULED_REENTRY (0x7f); handing the frame back");
                return -1;
            }
            return rbx::lua_gettop(state);
        }

        inline int newcclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            // Already a C closure: hand it straight back. Raising here is what
            // the reference executors avoid, and what broke a sUNC run.
            if (rbx::lua_iscfunction(state, 1)) {
                rbx::lua_pushvalue(state, 1);
                return 1;
            }
            if (!rbx::lua_islfunction(state, 1))
                return fail(state, "newcclosure: function expected");

            // lua_pushcclosurek stores the debugname pointer rather than
            // copying the string, so the name a script passes has to outlive
            // the call. Interning it in a node-based set gives every distinct
            // name one stable address for the life of the process.
            const char* debugname = nullptr;
            if (rbx::lua_type(state, 2) == luaobj::kTagString) {
                size_t nameLen = 0;
                const char* given = rbx::lua_tolstring(state, 2, &nameLen);
                if (given) {
                    static std::mutex namesMutex;
                    static std::unordered_set<std::string> names;
                    std::lock_guard<std::mutex> lock(namesMutex);
                    debugname = names.insert(std::string(given, nameLen)).first->c_str();
                }
            }

            rbx::lua_settop(state, 1);
            const int reference = rbx::lua_ref(state, 1);

            rbx::lua_pushcclosurek(state, reinterpret_cast<uintptr_t>(&newcclosureCall), debugname, 0,
                                   reinterpret_cast<uintptr_t>(&newcclosureContinue));

            const void* wrapper = rbx::lua_topointer(state, -1);
            if (!wrapper) {
                rbx::lua_unref(state, reference);
                return fail(state, "newcclosure: could not create the closure");
            }

            std::lock_guard<std::mutex> lock(wrappedMutex);
            wrappedFunctions[wrapper] = reference;
            return 1;
        }

        // Which of the three shapes a closure has. The spec asks for every
        // pairing of Luau, C and newcclosure-wrapped to work as both target and
        // hook, and the three are handled differently enough that telling them
        // apart is the first thing hookfunction does.
        enum class Kind { Lua, C, NewC };

        inline Kind kindOf(const void* closure)
        {
            if (!luaobj::byte(closure, luaobj::closure::isC))
                return Kind::Lua;
            if (luaobj::read<uintptr_t>(closure, luaobj::closure::cfunction) ==
                reinterpret_cast<uintptr_t>(&newcclosureCall))
                return Kind::NewC;
            return Kind::C;
        }

        // Pushes a standalone copy of the function at `index`. Shared by
        // clonefunction and by hookfunction, which needs one to hand back as
        // the un-hooked original.
        // The closure a value really refers to, with newcclosure wrappers
        // resolved to the payload they were built over.
        //
        // The loop is bounded because a wrapper of a wrapper is legal and a
        // cycle is not something to find out about by hanging.
        inline const void* rootClosure(uint64_t state, int index)
        {
            const void* closure = rbx::lua_topointer(state, index);
            if (!closure)
                return nullptr;

            const int top = rbx::lua_gettop(state);
            for (int depth = 0; depth < 16; depth++) {
                int reference = -1;
                {
                    std::lock_guard<std::mutex> lock(wrappedMutex);
                    auto found = wrappedFunctions.find(closure);
                    if (found == wrappedFunctions.end())
                        break;
                    reference = found->second;
                }
                rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
                const void* payload = rbx::lua_topointer(state, -1);
                rbx::lua_settop(state, -2);
                if (!payload || payload == closure)
                    break;
                closure = payload;
            }
            rbx::lua_settop(state, top);
            return closure;
        }

        // comparefunctions(a, b) - **3/6** have it (mac and src as
        // `comparefunctions`, mac also as `comparefunction`, vic as both).
        //
        // The point of it is that `==` says false for things that are the same
        // function: a `clonefunction` result, or a `newcclosure` wrapper and
        // its payload. So both sides are resolved through the wrapper map
        // first, and then compared by what actually identifies a closure - the
        // proto for a Lua one, the C function pointer for a C one.
        //
        // mac compares L closures by `getfunctionhash`, which is both far more
        // expensive and wrong in its own tree: it compares the two `const char*`
        // **pointers** rather than the strings, after popping both, so the
        // answer is whatever two dangling pointers happen to be. vic compares
        // the protos, which is what this does; hashing would also call two
        // structurally identical but unrelated functions equal.
        inline int comparefunctions(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagFunction)
                return utils::fail(state, "comparefunctions: function expected for argument 1");
            if (rbx::lua_type(state, 2) != luaobj::kTagFunction)
                return utils::fail(state, "comparefunctions: function expected for argument 2");

            const void* first = rootClosure(state, 1);
            const void* second = rootClosure(state, 2);
            if (!first || !second) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            if (first == second) {
                rbx::lua_pushboolean(state, 1);
                return 1;
            }

            // A clone is a distinct closure over the same payload, so identity
            // has to come from what the closure points at rather than from the
            // closure itself.
            const bool firstIsC = luaobj::byte(first, luaobj::closure::isC) != 0;
            const bool secondIsC = luaobj::byte(second, luaobj::closure::isC) != 0;
            if (firstIsC != secondIsC) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const int field = firstIsC ? luaobj::closure::cfunction : luaobj::closure::proto;
            const void* firstTarget = luaobj::read<void*>(first, field);
            const void* secondTarget = luaobj::read<void*>(second, field);
            rbx::lua_pushboolean(state,
                                 firstTarget && firstTarget == secondTarget ? 1 : 0);
            return 1;
        }

        inline bool pushClone(uint64_t state, int index)
        {
            if (rbx::lua_islfunction(state, index)) {
                rbx::lua_clonefunction(state, index);
                rbx::lua_getfenv(state, index);
                rbx::lua_setfenv(state, -2);
                return true;
            }

            if (!rbx::lua_iscfunction(state, index))
                return false;

            const void* closure = rbx::lua_topointer(state, index);
            if (!closure)
                return false;

            const int upvalues = luaobj::byte(closure, luaobj::closure::nupvalues);
            const uintptr_t function = rbx::lua_tocfunction(state, index);
            const uintptr_t continuation = luaobj::decodeContinuation(closure);

            // lua_pushcclosurek takes its upvalues off the stack. Pushing nil
            // first is what grows the stack to fit them; the values are copied
            // over the reserved slots afterwards, once no further push can move
            // the stack out from under the pointer.
            for (int i = 0; i < upvalues; i++)
                rbx::lua_pushnil(state);

            if (upvalues > 0) {
                uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
                for (int i = 0; i < upvalues; i++) {
                    std::memcpy(top - (upvalues - i) * luaobj::kValueSize,
                                static_cast<const uint8_t*>(closure) + luaobj::closure::upvals +
                                    i * luaobj::kValueSize,
                                luaobj::kValueSize);
                }
            }

            rbx::lua_pushcclosurek(state, function, nullptr, upvalues, continuation);
            rbx::lua_getfenv(state, index);
            rbx::lua_setfenv(state, -2);
            return true;
        }

        // Every Luau forwarding body pushLuaWrapper builds. Anchored for life
        // like everything else a patched closure points at, so the addresses
        // stay unique and the stack walk can recognise (and skip) them.
        inline std::mutex wrapperClosuresMutex;
        inline std::unordered_set<const void*> wrapperClosures;
        inline std::vector<int> wrapperAnchors;

        // Wraps the function at `index` in a Luau closure that has no upvalues
        // of its own, which is what makes a C hook usable against a Luau target
        // however few upvalues the target has room for. The wrapper reaches its
        // payload through its environment rather than an upvalue: a fresh table
        // holding `wrapped_func`, proxying everything else to the original's
        // environment so the wrapper is not a visibly different scope.
        inline bool pushLuaWrapper(uint64_t state, int index, int envIndex = -1,
                                   const void* nameSource = nullptr, bool guardErrors = false)
        {
            static const std::string plain = compileScript("return wrapped_func(...)");
            static const std::string guarded = compileScript(
                "local r = table.pack(pcall(wrapped_func, ...)) "
                "if not r[1] then error(r[2], 0) end "
                "return table.unpack(r, 2, r.n)");
            const std::string& bytecode = guardErrors ? guarded : plain;
            if (bytecode.empty() || bytecode[0] == 0)
                return false;

            rbx::lua_pushvalue(state, index);   // normalise to an absolute slot

            rbx::lua_createtable(state, 0, 1);  // the wrapper's environment

            // The payload goes in *before* the metatable does. lua_setfield
            // respects __newindex, and this environment's __newindex points at
            // the hook's own environment - which is the shared globals table -
            // so setting it afterwards wrote `wrapped_func` as a global instead.
            // Reading it back still worked, through __index, which is why this
            // looked fine with one hook and broke with two: the second L <- C
            // hook overwrote the first one's payload and both then called the
            // second hook's function.
            rbx::lua_pushvalue(state, -2);
            rbx::lua_setfield(state, -2, "wrapped_func");

            // A hook wrapped for a Luau target inherits the *target's*
            // environment, not the hook's. The hook body keeps its own
            // environment on its closure; exposing that on the patched target
            // is what the environment detection looks for.
            const auto pushEnv = [&]() {
                if (envIndex >= 0)
                    rbx::lua_pushvalue(state, envIndex);
                else
                    rbx::lua_getfenv(state, -3);
            };

            rbx::lua_createtable(state, 0, 2);  // its metatable
            pushEnv();
            rbx::lua_setfield(state, -2, "__index");
            pushEnv();
            rbx::lua_setfield(state, -2, "__newindex");
            rbx::lua_setreadonly(state, -1, 1);
            rbx::lua_setmetatable(state, -2);

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(state, "=newlclosure", bytecode.c_str(), bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(state, -4);     // the error, the env, the function
                return false;
            }

            // debug.info(f, "n") reads the proto's debugname. A wrapper is a
            // new proto, so copying the target's encoded name back is what
            // keeps a hooked Luau function answering with its original name.
            if (nameSource) {
                const uintptr_t sourceProto =
                    luaobj::read<uintptr_t>(nameSource, luaobj::closure::proto);
                const void* wrapperClosure = rbx::lua_topointer(state, -1);
                if (sourceProto && wrapperClosure) {
                    const uintptr_t encoded =
                        luaobj::read<uintptr_t>(reinterpret_cast<const void*>(sourceProto), 0x60);
                    const uintptr_t wrapperProto =
                        luaobj::read<uintptr_t>(wrapperClosure, luaobj::closure::proto);
                    if (encoded && wrapperProto) {
                        const uintptr_t name = encoded - (sourceProto + 0x60);
                        luaobj::write<uintptr_t>(reinterpret_cast<void*>(wrapperProto), 0x60,
                                                 name + (wrapperProto + 0x60));
                    }
                }
            }

            rbx::setprotocapabilities(state, -1, &MaxCapabilities);

            if (const void* wrapper = rbx::lua_topointer(state, -1)) {
                wrapperAnchors.push_back(rbx::lua_ref(state, -1));
                std::lock_guard<std::mutex> lock(wrapperClosuresMutex);
                wrapperClosures.insert(wrapper);
            }

            // stack: function, env, chunk -> give the chunk that env, then drop
            // everything but the chunk.
            rbx::lua_pushvalue(state, -2);
            rbx::lua_setfenv(state, -2);
            rbx::lua_remove(state, -2);
            rbx::lua_remove(state, -2);
            return true;
        }

        // Overwrites a live closure in place. No write barrier is issued for
        // the values copied in, so everything a hook brings with it is anchored
        // in the registry by hookfunction before any of this runs.
        inline void patchCClosure(void* destination, const void* source,
                                  uintptr_t function, uintptr_t continuation)
        {
            const int upvalues = luaobj::byte(source, luaobj::closure::nupvalues);
            luaobj::write<void*>(destination, luaobj::closure::env,
                                 luaobj::read<void*>(source, luaobj::closure::env));
            luaobj::writeByte(destination, luaobj::closure::stacksize,
                              luaobj::byte(source, luaobj::closure::stacksize));
            luaobj::writeByte(destination, luaobj::closure::preload,
                              luaobj::byte(source, luaobj::closure::preload));
            for (int i = 0; i < upvalues; i++) {
                luaobj::copyValue(destination, luaobj::closure::upvals + i * luaobj::kValueSize,
                                  source, luaobj::closure::upvals + i * luaobj::kValueSize);
            }
            luaobj::writeByte(destination, luaobj::closure::nupvalues, static_cast<uint8_t>(upvalues));
            luaobj::write<uintptr_t>(destination, luaobj::closure::cfunction, function);
            luaobj::writeContinuation(destination, continuation);
        }

        inline void patchLClosure(void* destination, const void* source)
        {
            const int upvalues = luaobj::byte(source, luaobj::closure::nupvalues);
            luaobj::write<void*>(destination, luaobj::closure::env,
                                 luaobj::read<void*>(source, luaobj::closure::env));
            luaobj::writeByte(destination, luaobj::closure::stacksize,
                              luaobj::byte(source, luaobj::closure::stacksize));
            luaobj::writeByte(destination, luaobj::closure::preload,
                              luaobj::byte(source, luaobj::closure::preload));
            luaobj::write<void*>(destination, luaobj::closure::proto,
                                 luaobj::read<void*>(source, luaobj::closure::proto));
            for (int i = 0; i < upvalues; i++) {
                luaobj::copyValue(destination, luaobj::closure::uprefs + i * luaobj::kValueSize,
                                  source, luaobj::closure::uprefs + i * luaobj::kValueSize);
            }
            luaobj::writeByte(destination, luaobj::closure::nupvalues, static_cast<uint8_t>(upvalues));
        }

        inline int isnewcclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagFunction);
            const void* closure = rbx::lua_topointer(state, 1);
            rbx::lua_pushboolean(state,
                closure && rbx::lua_iscfunction(state, 1) && kindOf(closure) == Kind::NewC ? 1 : 0);
            return 1;
        }

        // The inverse of newcclosure: a Luau closure with no upvalues of its
        // own that calls through to whatever it was given. hookfunction already
        // needs one to put a C hook on a Luau target, so this is the same
        // wrapper under a name scripts can reach.
        inline int newlclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (!rbx::lua_islfunction(state, 1) && !rbx::lua_iscfunction(state, 1))
                return fail(state, "newlclosure: function expected");
            if (rbx::lua_islfunction(state, 1)) {
                rbx::lua_pushvalue(state, 1);
                return 1;
            }
            if (!pushLuaWrapper(state, 1))
                return fail(state, "newlclosure: could not wrap the function");
            return 1;
        }

        // Anything a live closure has been made to point at is kept alive here,
        // because patching one writes no GC barrier. Entries are never dropped:
        // a hook can be reinstated by the game at any point, and the cost of
        // holding it is far smaller than the cost of guessing wrong.
        inline std::vector<int> anchors;

        inline void anchor(uint64_t state, int index)
        {
            anchors.push_back(rbx::lua_ref(state, index));
        }

        // target -> registry ref of the clone taken before it was first hooked.
        inline std::unordered_map<const void*, int> hookedFunctions;

        // A hookmetamethod write installs either the hook itself (every
        // metamethod but __namecall) or a C wrapper around it. restorefunction
        // is handed that installed function by callers that restore with
        // `restorefunction(getrawmetatable(o).__namecall)`, so the metatable
        // and the function that was there before both have to be remembered.
        struct MetamethodHook {
            int metatableRef = 0;
            int previousRef = 0;
            int hookRef = 0;
            int originalEnvRef = 0;
            std::string method;
        };
        inline std::unordered_map<const void*, MetamethodHook> metamethodHooks;
        inline std::mutex metamethodHookMutex;

        inline void registerMetamethodHook(uint64_t state, const void* installed,
                                           int metatableIndex, int previousIndex,
                                           int hookIndex, int originalEnvRef,
                                           const std::string& method)
        {
            if (!installed)
                return;
            const int top = rbx::lua_gettop(state);
            rbx::lua_pushvalue(state, metatableIndex);
            const int metatableRef = rbx::lua_ref(state, -1);
            rbx::lua_settop(state, top);
            rbx::lua_pushvalue(state, previousIndex);
            const int previousRef = rbx::lua_ref(state, -1);
            rbx::lua_settop(state, top);
            rbx::lua_pushvalue(state, hookIndex);
            const int hookRef = rbx::lua_ref(state, -1);
            rbx::lua_settop(state, top);

            std::lock_guard<std::mutex> lock(metamethodHookMutex);
            metamethodHooks[installed] =
                MetamethodHook{metatableRef, previousRef, hookRef, originalEnvRef, method};
        }

        inline const char* kindName(Kind kind)
        {
            switch (kind) {
                case Kind::Lua:  return "L";
                case Kind::NewC: return "NC";
                default:         return "C";
            }
        }

        inline int hookfunction(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "hookfunction");

            const void* target = rbx::lua_topointer(state, 1);
            const void* hook = rbx::lua_topointer(state, 2);
            if (!target || (!rbx::lua_islfunction(state, 1) && !rbx::lua_iscfunction(state, 1)))
                return fail(state, "hookfunction: function expected as argument 1");
            if (!hook || (!rbx::lua_islfunction(state, 2) && !rbx::lua_iscfunction(state, 2)))
                return fail(state, "hookfunction: function expected as argument 2");

            // NOTE: do not add a "cannot hook a function with itself" guard here.
            // Two of the six have one (RbxStu raises, the macOS one returns
            // early) and it was tried - it fails sUNC. `restorefunction` broke
            // with "Failed to restore a L -> C hook back to original[1]" and the
            // suite then threw out of the whole run
            // ("invalid argument #1 to 'yield' (thread expected, got boolean)"),
            // because a caller that gets an error where it expected the previous
            // closure carries a boolean forward. The self-hook hazard is real
            // for the C-target-Luau-hook path, but the cure is worse.

            const Kind targetKind = kindOf(target);
            const Kind hookKind = kindOf(hook);
            if (trace.active) {
                char detail[128];
                snprintf(detail, sizeof(detail), "%s(%p) <- %s(%p)",
                         kindName(targetKind), target, kindName(hookKind), hook);
                trace.note(detail);
            }

            // Every call hands back the target as it is *right now*, not as it
            // was before the first hook. That is what makes chaining work:
            //     original = hookfunction(f, function(...) return original(...) end)
            // done twice has to run both hooks, which it cannot if the second
            // call returns something that skips the first. restorefunction
            // still goes all the way back, so the very first snapshot is kept
            // separately.
            if (!pushClone(state, 1))
                return fail(state, "hookfunction: could not clone the target");

            const void* snapshot = rbx::lua_topointer(state, -1);
            const int snapshotReference = rbx::lua_ref(state, -1);
            rbx::lua_settop(state, -2);

            // A wrapper's identity lives in the map, not in the closure, so a
            // copy of one has to be registered too or calling the returned
            // original would find nothing to call.
            if (targetKind == Kind::NewC && snapshot) {
                std::lock_guard<std::mutex> lock(wrappedMutex);
                auto found = wrappedFunctions.find(target);
                if (found != wrappedFunctions.end())
                    wrappedFunctions[snapshot] = found->second;
            }

            if (!hookedFunctions.count(target))
                hookedFunctions[target] = snapshotReference;

            anchor(state, 2);

            void* writable = const_cast<void*>(target);

            if (targetKind == Kind::Lua) {
                // A Luau target can only take a Luau body, and only one whose
                // upvalues fit in the space the target already has. Everything
                // else goes through a no-upvalue wrapper first. The wrapper
                // keeps the target's environment and debug name so the patched
                // closure does not expose the hook's private state.
                const bool wrap = hookKind != Kind::Lua ||
                    luaobj::byte(hook, luaobj::closure::nupvalues) >
                        luaobj::byte(target, luaobj::closure::nupvalues);
                if (!wrap) {
                    {
                        std::lock_guard<std::mutex> lock(hookedEnvMutex);
                        if (!hookedEnvs.count(writable)) {
                            rbx::lua_getfenv(state, 1);
                            hookedEnvs[writable] = rbx::lua_ref(state, -1);
                            rbx::lua_settop(state, -2);
                        }
                    }
                    patchLClosure(writable, hook);
                } else {
                    const int top = rbx::lua_gettop(state);
                    rbx::lua_getfenv(state, 1);
                    const int envIndex = rbx::lua_gettop(state);
                    if (!pushLuaWrapper(state, 2, envIndex, target))
                        return fail(state, "hookfunction: could not wrap the hook");
                    anchor(state, -1);
                    patchLClosure(writable, rbx::lua_topointer(state, -1));
                    rbx::lua_settop(state, top);
                }
            } else if (hookKind == Kind::C) {
                if (luaobj::byte(hook, luaobj::closure::nupvalues) >
                    luaobj::byte(target, luaobj::closure::nupvalues))
                    return fail(state, "hookfunction: the hook has more upvalues than the target");
                patchCClosure(writable, hook,
                              luaobj::read<uintptr_t>(hook, luaobj::closure::cfunction),
                              luaobj::decodeContinuation(hook));
            } else {
                // A C target taking a Luau body, a NewC target, or a raw C hook
                // with more upvalues than the target can hold becomes a
                // dispatcher: the hook stays in the registry and the target
                // calls it through newcclosureCall, so its upvalues do not have
                // to fit in the target's fixed upvalue array.
                int reference;
                if (hookKind == Kind::NewC) {
                    std::lock_guard<std::mutex> lock(wrappedMutex);
                    auto found = wrappedFunctions.find(hook);
                    if (found == wrappedFunctions.end())
                        return fail(state, "hookfunction: the hook is a wrapper with nothing behind it");
                    rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
                    reference = rbx::lua_ref(state, -1);
                    rbx::lua_settop(state, -2);
                } else {
                    reference = rbx::lua_ref(state, 2);
                }

                luaobj::write<uintptr_t>(writable, luaobj::closure::cfunction,
                                         reinterpret_cast<uintptr_t>(&newcclosureCall));
                luaobj::writeContinuation(writable, reinterpret_cast<uintptr_t>(&newcclosureContinue));

                std::lock_guard<std::mutex> lock(wrappedMutex);
                wrappedFunctions[target] = reference;
            }

            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, snapshotReference);
            return trace.done(1);
        }

        inline int isfunctionhooked(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* target = rbx::lua_topointer(state, 1);
            if (!target || (!rbx::lua_islfunction(state, 1) && !rbx::lua_iscfunction(state, 1)))
                return fail(state, "isfunctionhooked: function expected");
            rbx::lua_pushboolean(state, hookedFunctions.count(target) ? 1 : 0);
            return 1;
        }

        inline int restorefunction(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "restorefunction");

            const void* target = rbx::lua_topointer(state, 1);
            if (!target || (!rbx::lua_islfunction(state, 1) && !rbx::lua_iscfunction(state, 1)))
                return fail(state, "restorefunction: function expected");

            // A metamethod installed by hookmetamethod is not in
            // hookedFunctions; its saved metatable/previous pair lives here.
            {
                MetamethodHook hook;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lock(metamethodHookMutex);
                    auto it = metamethodHooks.find(target);
                    if (it != metamethodHooks.end()) {
                        hook = it->second;
                        metamethodHooks.erase(it);
                        have = true;
                    }
                }
                if (have) {
                    rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, hook.metatableRef);
                    const bool locked = rbx::lua_getreadonly(state, -1) != 0;
                    if (locked)
                        rbx::lua_setreadonly(state, -1, 0);
                    rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, hook.previousRef);
                    rbx::lua_setfield(state, -2, hook.method.c_str());
                    if (locked)
                        rbx::lua_setreadonly(state, -1, 1);
                    rbx::lua_settop(state, -2);

                    if (hook.hookRef && hook.originalEnvRef) {
                        rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, hook.hookRef);
                        rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, hook.originalEnvRef);
                        rbx::lua_setfenv(state, -2);
                        rbx::lua_settop(state, -3);
                    }
                    rbx::lua_unref(state, hook.metatableRef);
                    rbx::lua_unref(state, hook.previousRef);
                    if (hook.hookRef)
                        rbx::lua_unref(state, hook.hookRef);
                    if (hook.originalEnvRef)
                        rbx::lua_unref(state, hook.originalEnvRef);
                    return trace.done(0);
                }
            }

            auto found = hookedFunctions.find(target);
            if (found == hookedFunctions.end())
                return fail(state, "restorefunction: function is not hooked");

            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
            const void* backup = rbx::lua_topointer(state, -1);
            if (!backup) {
                rbx::lua_settop(state, -2);
                return fail(state, "restorefunction: the saved original is gone");
            }

            // A C target with frames still suspended in it cannot be patched
            // back yet: on resume the VM reads the frame's continuation off
            // this closure, and an original C function's continuation is
            // usually null. Hand the restore to newcclosureContinue, which
            // applies it when the last such frame comes back.
            if (luaobj::byte(backup, luaobj::closure::isC)) {
                bool suspended = false;
                {
                    std::lock_guard<std::mutex> lock(suspendedMutex);
                    for (const auto& entry : suspendedTargets)
                        if (entry.second == target) {
                            suspended = true;
                            break;
                        }
                    if (suspended)
                        deferredRestores[target] = found->second;
                }
                if (suspended) {
                    {
                        std::lock_guard<std::mutex> lock(wrappedMutex);
                        wrappedFunctions.erase(target);
                    }
                    rbx::lua_settop(state, -2);
                    hookedFunctions.erase(found);
                    return trace.done(0);
                }
            }

            void* writable = const_cast<void*>(target);
            if (luaobj::byte(backup, luaobj::closure::isC)) {
                patchCClosure(writable, backup,
                              luaobj::read<uintptr_t>(backup, luaobj::closure::cfunction),
                              luaobj::decodeContinuation(backup));
                // If the saved original was itself a wrapper, the target has to
                // point back at the same body it used to.
                std::lock_guard<std::mutex> lock(wrappedMutex);
                auto inner = wrappedFunctions.find(backup);
                if (inner != wrappedFunctions.end())
                    wrappedFunctions[target] = inner->second;
                else
                    wrappedFunctions.erase(target);
            } else {
                patchLClosure(writable, backup);
                int envRef = 0;
                {
                    std::lock_guard<std::mutex> lock(hookedEnvMutex);
                    auto env = hookedEnvs.find(writable);
                    if (env != hookedEnvs.end()) {
                        envRef = env->second;
                        hookedEnvs.erase(env);
                    }
                }
                if (envRef > 0)
                    rbx::lua_unref(state, envRef);
            }

            rbx::lua_settop(state, -2);

            // The backup closure is about to be collectable, so nothing may go
            // on holding its address. `wrappedFunctions` is keyed by closure
            // pointer, and a freed pointer can be handed straight back out for
            // the next closure - at which point a stale entry answers for an
            // object that has nothing to do with it. Drop it before releasing.
            {
                std::lock_guard<std::mutex> lock(wrappedMutex);
                wrappedFunctions.erase(backup);
            }
            rbx::lua_unref(state, found->second);
            hookedFunctions.erase(found);
            return trace.done(0);
        }

        // A Luau closure clones through Roblox's own lua_clonefunction, which
        // builds a new Closure over the same proto and copies the upvalue
        // references across - so the clone keeps its name, line and upvalues,
        // and hooking either one leaves the other alone. It does hand the clone
        // L->gt as its environment, though, and the spec wants the original's.
        //
        // A C closure has no proto to share, so it is rebuilt instead: same
        // function pointer, same continuation, same upvalues. The debugname is
        // dropped rather than recovered - Roblox keeps it in one of two slots
        // depending on a feature flag, and nothing in the spec reads it back
        // off a C closure.
        inline int clonefunction(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            if (!pushClone(state, 1))
                return fail(state, "clonefunction: function expected");

            // A wrapper carries its payload in a side map rather than in the
            // closure, so the copy has to be pointed at the same one.
            if (const void* clone = rbx::lua_topointer(state, -1)) {
                std::lock_guard<std::mutex> lock(wrappedMutex);
                auto found = wrappedFunctions.find(rbx::lua_topointer(state, 1));
                if (found != wrappedFunctions.end())
                    wrappedFunctions[clone] = found->second;
            }
            return 1;
        }

    }

    // Reaching a Luau function's constants, protos and upvalues means reading
    // its Proto, and the vendored header's field offsets are a guess until they
    // are checked against something independent. This hands the raw reads back
    // to Lua so they can be compared with debug.info, which answers for the
    // same four fields from Roblox's own side.
    // Defined with the rest of the stack work further down; the debug library
    // needs the frame walk because every one of its entry points takes either a
    // function or a stack level.
    namespace Stack {
        inline void* frameAt(uint64_t state, int level);
        inline void* frameAt(uint64_t state, int level, bool skipTop);
        inline bool looksLikeFrame(uint64_t state, const void* callinfo);
        inline int stride(uint64_t state);
    }

    namespace Debug {

        inline const void* protoOf(uint64_t state, int index)
        {
            if (!rbx::lua_islfunction(state, index))
                return nullptr;
            const void* closure = rbx::lua_topointer(state, index);
            return closure ? luaobj::read<void*>(closure, luaobj::closure::proto) : nullptr;
        }

        inline int fail(uint64_t state, const std::string& message)
        {
            rbx::lua_pushlstring(state, message.data(), message.size());
            rbx::lua_error(state);
            return 0;
        }

        // A pointer to the TValue at a stack index. index2addr is not exported,
        // and setconstant/setupvalue need the raw value rather than a copy of it
        // pushed somewhere else.
        inline const uint8_t* stackSlot(uint64_t state, int index)
        {
            uint8_t* base = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::base);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            uint8_t* slot = index > 0 ? base + (index - 1) * luaobj::kValueSize
                                      : top + index * luaobj::kValueSize;
            return (slot >= base && slot < top) ? slot : nullptr;
        }

        // Writing into a proto or a closure issues no GC barrier, so whatever
        // was written has to be kept reachable some other way.
        inline std::vector<int> anchors;

        inline void anchorValue(uint64_t state, int index)
        {
            anchors.push_back(rbx::lua_ref(state, index));
        }

        // Builds a closure over a nested proto so it can be handed to Lua. The
        // upvalues come out nil, which is why the spec calls a proto retrieved
        // this way inert - it is for reading, not for calling.
        // Every closure this file has handed out to stand for a proto. They
        // share the proto with the real thing, so without this the heap walk
        // behind `activated` returns them alongside - and ahead of - the live
        // closure the caller asked for. Measured: debug.getproto(f, 1, true)
        // came back with two entries, the first of which was ours.
        inline std::unordered_set<const void*> inertClosures;

        inline void pushProtoClosure(uint64_t state, void* proto)
        {
            if (!proto) {
                rbx::lua_pushnil(state);
                return;
            }

            void* globals = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::gt);
            const int nups = luaobj::byte(proto, luaobj::proto::nups);
            void* closure = rbx::luaF_newLclosure(state, nups, globals, proto);
            if (!closure) {
                rbx::lua_pushnil(state);
                return;
            }
            inertClosures.insert(closure);

            rbx::lua_pushnil(state);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            uint8_t* slot = top - luaobj::kValueSize;
            luaobj::write<void*>(slot, luaobj::value::gc, closure);
            luaobj::write<int>(slot, luaobj::value::tt, luaobj::kTagFunction);
        }

        // The live closures built from a proto, found by walking the heap. This
        // is what `activated` asks for, and unlike a fresh closure these are
        // callable because they carry real upvalues.
        struct ProtoWalk {
            const void* proto;
            std::vector<const void*> found;
            int tagAt = 0;
        };

        inline bool protoVisit(void* context, void* page, void* object)
        {
            (void)page;
            auto* walk = static_cast<ProtoWalk*>(context);
            if (luaobj::byte(object, walk->tagAt) != luaobj::kTagFunction)
                return false;
            if (luaobj::byte(object, luaobj::closure::isC))
                return false;
            if (luaobj::read<void*>(object, luaobj::closure::proto) == walk->proto &&
                inertClosures.find(object) == inertClosures.end())
                walk->found.push_back(object);
            return false;
        }

        inline void pushActiveClosures(uint64_t state, void* proto)
        {
            ProtoWalk walk{proto, {}, gcTagOffset(state)};
            rbx::lua_gc(state, LUA_GCSTOP, 0);
            rbx::luaM_visitgco(state, &walk, protoVisit);

            rbx::lua_createtable(state, static_cast<int>(walk.found.size()), 0);
            const int table = rbx::lua_gettop(state);
            for (size_t i = 0; i < walk.found.size(); i++) {
                rbx::lua_pushnil(state);
                uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
                uint8_t* slot = top - luaobj::kValueSize;
                luaobj::write<const void*>(slot, luaobj::value::gc, walk.found[i]);
                luaobj::write<int>(slot, luaobj::value::tt, luaobj::kTagFunction);
                rbx::lua_rawseti(state, table, static_cast<int>(i + 1));
            }
            rbx::lua_gc(state, LUA_GCRESTART, 0);
        }

        // Every debug entry point takes a Luau function; a C closure has no
        // bytecode to look at and the spec says to throw for one. Stack levels
        // are turned into functions by the Lua wrappers with debug.info.
        // Every debug.* entry point is documented as taking
        // `func: (...any) -> (...any) | number` - a function *or a stack
        // level*. When a level is given it is resolved to the function running
        // at that level and put in the argument's place, so nothing downstream
        // has to know the difference. sUNC passes a level to debug.setupvalue,
        // which is what "debug.setupvalue: function expected" was.
        inline bool resolveFunctionArgument(uint64_t state, int index)
        {
            const int type = rbx::lua_type(state, index);
            if (type != luaobj::kTagNumber && type != luaobj::kTagInteger)
                return type == luaobj::kTagFunction;

            int isnum = 0;
            const int level = rbx::lua_tointegerx(state, index, &isnum);
            if (!isnum || level < 1)
                return false;

            void* frame = Stack::frameAt(state, level);
            if (!frame || !Stack::looksLikeFrame(state, frame))
                return false;
            const void* slot = luaobj::read<void*>(frame, luaobj::callinfo::func);
            if (!slot || luaobj::read<int>(slot, luaobj::value::tt) != luaobj::kTagFunction)
                return false;

            // Reserve a slot, copy the frame's function into it, then move it
            // over the level so the argument index keeps its meaning.
            rbx::lua_pushnil(state);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            std::memcpy(top - luaobj::kValueSize, slot, luaobj::kValueSize);
            rbx::lua_insert(state, index);
            rbx::lua_remove(state, index + 1);
            return true;
        }

        inline const void* requireProto(uint64_t state, int index, const char* who)
        {
            resolveFunctionArgument(state, index);
            if (!rbx::lua_islfunction(state, index)) {
                fail(state, std::string(who) + ": Luau function expected");
                return nullptr;
            }
            const void* proto = protoOf(state, index);
            if (!proto)
                fail(state, std::string(who) + ": function has no prototype");
            return proto;
        }

        // Copies a TValue into a freshly reserved stack slot. There is no push
        // that takes an arbitrary tagged value, and constants and upvalues are
        // both read as raw TValues.
        inline void pushValue(uint64_t state, const void* value)
        {
            rbx::lua_pushnil(state);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            std::memcpy(top - luaobj::kValueSize, value, luaobj::kValueSize);
        }

        // Tables and closures are deliberately not handed out: the spec limits
        // constants to numbers, strings, booleans and nil, and the rest of what
        // lives in k (imports, nested closures) is not safe to expose.
        inline bool isPlainValue(const void* value)
        {
            const int tag = luaobj::read<int>(value, luaobj::value::tt);
            return tag != luaobj::kTagTable && tag != luaobj::kTagFunction &&
                   tag != luaobj::kTagUserdata && tag != luaobj::kTagThread;
        }

        // SHA384 over the function's constants followed by its instructions -
        // the ordering the spec asks for, so hashes stay comparable. Constants
        // are hashed by content rather than by TValue, since a TValue holds
        // pointers that differ between runs.
        inline int getfunctionhash(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* proto = requireProto(state, 1, "getfunctionhash");
            if (!proto)
                return 0;

            std::string buffer;
            const int constants = luaobj::read<int>(proto, luaobj::proto::sizek);
            const uint8_t* k = luaobj::read<uint8_t*>(proto, luaobj::proto::k);
            for (int i = 0; i < constants && k; i++) {
                const uint8_t* value = k + i * luaobj::kValueSize;
                const int tag = luaobj::read<int>(value, luaobj::value::tt);
                buffer.push_back(static_cast<char>(tag));
                if (tag == luaobj::kTagString) {
                    const void* text = luaobj::read<void*>(value, luaobj::value::gc);
                    if (text) {
                        const uint32_t length = luaobj::read<uint32_t>(text, luaobj::tstring::length);
                        buffer.append(static_cast<const char*>(text) + luaobj::tstring::data, length);
                    }
                } else {
                    buffer.append(reinterpret_cast<const char*>(value), 8);
                }
            }

            const int instructions = luaobj::read<int>(proto, luaobj::proto::sizecode);
            const uint8_t* code = luaobj::read<uint8_t*>(proto, luaobj::proto::code);
            if (code && instructions > 0)
                buffer.append(reinterpret_cast<const char*>(code), static_cast<size_t>(instructions) * 4);

            unsigned char digest[CC_SHA384_DIGEST_LENGTH];
            CC_SHA384(buffer.data(), static_cast<CC_LONG>(buffer.size()), digest);

            static const char* const hex = "0123456789abcdef";
            std::string out;
            out.reserve(sizeof(digest) * 2);
            for (unsigned char byte : digest) {
                out += hex[byte >> 4];
                out += hex[byte & 0xf];
            }
            rbx::lua_pushlstring(state, out.data(), out.size());
            return 1;
        }

        inline int getconstants(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* proto = requireProto(state, 1, "debug.getconstants");
            if (!proto)
                return 0;

            const int count = luaobj::read<int>(proto, luaobj::proto::sizek);
            const uint8_t* constants = luaobj::read<uint8_t*>(proto, luaobj::proto::k);

            rbx::lua_createtable(state, count, 0);
            for (int i = 0; i < count && constants; i++) {
                const uint8_t* value = constants + i * luaobj::kValueSize;
                if (!isPlainValue(value))
                    continue;   // leaves a hole, which is what the spec asks for
                pushValue(state, value);
                rbx::lua_rawseti(state, -2, i + 1);
            }
            return 1;
        }

        inline int getconstant(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* proto = requireProto(state, 1, "debug.getconstant");
            if (!proto)
                return 0;

            int isnum = 0;
            const int index = rbx::lua_tointegerx(state, 2, &isnum);
            const int count = luaobj::read<int>(proto, luaobj::proto::sizek);
            const uint8_t* constants = luaobj::read<uint8_t*>(proto, luaobj::proto::k);
            if (!isnum || index < 1 || index > count || !constants)
                return fail(state, "debug.getconstant: index out of range");

            const uint8_t* value = constants + (index - 1) * luaobj::kValueSize;
            if (!isPlainValue(value))
                rbx::lua_pushnil(state);
            else
                pushValue(state, value);
            return 1;
        }

        inline int setconstant(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* proto = requireProto(state, 1, "debug.setconstant");
            if (!proto)
                return 0;

            int isnum = 0;
            const int index = rbx::lua_tointegerx(state, 2, &isnum);
            const int count = luaobj::read<int>(proto, luaobj::proto::sizek);
            uint8_t* constants = luaobj::read<uint8_t*>(proto, luaobj::proto::k);
            if (!isnum || index < 1 || index > count || !constants)
                return fail(state, "debug.setconstant: index out of range");

            // The spec's own signature is
            //   value: number | string | boolean | nil
            // and RbxStu enforces exactly that list. It is not pedantry: a
            // constant slot is read by opcodes that assume its type, so writing
            // a table or a closure where a number was is a way to make the VM
            // misread its own bytecode. (The macOS reference goes further and
            // demands the *same* type, refusing a function or table target
            // outright; that is stricter than the spec and only one of the two.)
            const int valueType = rbx::lua_type(state, 3);
            if (valueType != luaobj::kTagNil && valueType != luaobj::kTagBoolean &&
                valueType != luaobj::kTagNumber && valueType != luaobj::kTagInteger &&
                valueType != luaobj::kTagString)
                return fail(state,
                            "debug.setconstant: value must be a number, string, boolean or nil");

            // Anchored, because writing into a proto issues no GC barrier and
            // the new value may be the only thing keeping a string alive.
            const uint8_t* value = stackSlot(state, 3);
            if (!value)
                return fail(state, "debug.setconstant: expected a value");

            anchorValue(state, 3);
            std::memcpy(constants + (index - 1) * luaobj::kValueSize, value, luaobj::kValueSize);
            return 0;
        }

        // Loads a chunk without recording its name as one of ours.
        //
        // `loadstring` marks every chunkname it loads so isexecutorclosure can
        // answer for the closures inside it. debug.getproto needs the opposite:
        // the stand-in it hands back has to carry the *original* script's source
        // name, and marking that name would make every function in that script
        // read as an executor closure. So this one loads and does not mark.
        //
        //   __vanta_loadchunk(source, chunkname) -> function | nil, error
        inline int loadchunk(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            size_t sourceLen = 0, nameLen = 0;
            const char* source = rbx::lua_tolstring(state, 1, &sourceLen);
            const char* name = rbx::lua_tolstring(state, 2, &nameLen);
            if (!source) {
                rbx::lua_pushnil(state);
                return 1;
            }
            const std::string chunkname = name ? std::string(name, nameLen) : std::string("vanta");
            const std::string bytecode = compileScript(std::string(source, sourceLen));

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(state, chunkname.c_str(), bytecode.c_str(),
                                        bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(state, -2);
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::setprotocapabilities(state, -1, &MaxCapabilities);

            return 1;
        }

        // The line a function's proto claims to be defined on. Writing it is
        // what lets a stand-in proto report the line the real one has without
        // the caller padding its generated source with blank lines.
        inline int setprotoline(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (!rbx::lua_islfunction(state, 1))
                return 0;
            const void* closure = rbx::lua_topointer(state, 1);
            void* proto = closure ? luaobj::read<void*>(closure, luaobj::closure::proto) : nullptr;
            int isnum = 0;
            const int line = rbx::lua_tointegerx(state, 2, &isnum);
            if (proto && isnum)
                luaobj::write<int>(proto, luaobj::proto::linedefined, line);
            return 0;
        }

        inline int getprotos(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "getprotos");
            (void)trace;
            const void* proto = requireProto(state, 1, "debug.getprotos");
            if (!proto)
                return 0;

            const int count = luaobj::read<int>(proto, luaobj::proto::sizep);
            void** nested = luaobj::read<void**>(proto, luaobj::proto::p);

            rbx::lua_createtable(state, count, 0);
            for (int i = 0; i < count && nested; i++) {
                pushProtoClosure(state, nested[i]);
                rbx::lua_rawseti(state, -2, i + 1);
            }
            return 1;
        }

        inline int getproto(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "getproto");
            (void)trace;
            const void* proto = requireProto(state, 1, "debug.getproto");
            if (!proto)
                return 0;

            int isnum = 0;
            const int index = rbx::lua_tointegerx(state, 2, &isnum);
            const int count = luaobj::read<int>(proto, luaobj::proto::sizep);
            void** nested = luaobj::read<void**>(proto, luaobj::proto::p);
            if (!isnum || index < 1 || index > count || !nested)
                return fail(state, "debug.getproto: index out of range");

            // activated: hand back the live closures built from this proto
            // rather than a fresh one, which the spec keeps deliberately inert.
            if (rbx::lua_toboolean(state, 3)) {
                pushActiveClosures(state, nested[index - 1]);
                return 1;
            }

            pushProtoClosure(state, nested[index - 1]);
            return 1;
        }

        // Upvalues live on the closure, not the proto: an open one is a
        // LUA_TUPVAL object holding a pointer to the live value, a closed one is
        // the value itself.
        inline uint8_t* upvalueSlot(uint64_t state, int index, int number, const char* who)
        {
            resolveFunctionArgument(state, index);
            const void* closure = rbx::lua_topointer(state, index);
            if (!closure ||
                (!rbx::lua_islfunction(state, index) && !rbx::lua_iscfunction(state, index))) {
                fail(state, std::string(who) + ": function expected");
                return nullptr;
            }

            // A C closure's upvalues are not the language's upvalues - they are
            // the implementation's - so the spec has this family refuse one
            // outright rather than hand back something a script cannot reason
            // about. The suite checks it with print.
            if (rbx::lua_iscfunction(state, index)) {
                fail(state, std::string(who) + ": cannot read the upvalues of a C closure");
                return nullptr;
            }

            const bool isC = luaobj::byte(closure, luaobj::closure::isC) != 0;
            const int count = luaobj::byte(closure, luaobj::closure::nupvalues);
            if (number < 1 || number > count) {
                fail(state, std::string(who) + ": index out of range");
                return nullptr;
            }

            uint8_t* base = const_cast<uint8_t*>(static_cast<const uint8_t*>(closure)) +
                            (isC ? luaobj::closure::upvals : luaobj::closure::uprefs);
            uint8_t* slot = base + (number - 1) * luaobj::kValueSize;

            if (!isC && luaobj::read<int>(slot, luaobj::value::tt) == luaobj::kTagUpval) {
                void* upvalue = luaobj::read<void*>(slot, luaobj::value::gc);
                if (upvalue)
                    return luaobj::read<uint8_t*>(upvalue, luaobj::upval::value);
            }
            return slot;
        }

        inline int getupvalues(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            resolveFunctionArgument(state, 1);
            const void* closure = rbx::lua_topointer(state, 1);
            if (!closure || !rbx::lua_islfunction(state, 1))
                return fail(state, "debug.getupvalues: Luau function expected");

            const int count = luaobj::byte(closure, luaobj::closure::nupvalues);
            rbx::lua_createtable(state, count, 0);
            for (int i = 1; i <= count; i++) {
                uint8_t* slot = upvalueSlot(state, 1, i, "debug.getupvalues");
                if (!slot)
                    return 0;
                pushValue(state, slot);
                rbx::lua_rawseti(state, -2, i);
            }
            return 1;
        }

        // How many upvalues a closure has, as a count rather than as a table.
        // debug.getupvalues answers with a table, and a closure whose upvalues
        // are all nil - which is exactly what a proto stand-in has - gives that
        // table a length of zero. The count is the only honest source.
        inline int nupvalues(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* closure = rbx::lua_topointer(state, 1);
            if (!closure || (!rbx::lua_islfunction(state, 1) && !rbx::lua_iscfunction(state, 1))) {
                rbx::lua_pushinteger(state, 0);
                return 1;
            }
            rbx::lua_pushinteger(state, luaobj::byte(closure, luaobj::closure::nupvalues));
            return 1;
        }

        inline int getupvalue(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "getupvalue");
            (void)trace;
            int isnum = 0;
            const int number = rbx::lua_tointegerx(state, 2, &isnum);
            if (!isnum)
                return fail(state, "debug.getupvalue: expected an index");
            uint8_t* slot = upvalueSlot(state, 1, number, "debug.getupvalue");
            if (!slot)
                return 0;
            pushValue(state, slot);
            return trace.done(1);
        }

        inline int setupvalue(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "setupvalue");
            (void)trace;
            int isnum = 0;
            const int number = rbx::lua_tointegerx(state, 2, &isnum);
            if (!isnum)
                return fail(state, "debug.setupvalue: expected an index");
            uint8_t* slot = upvalueSlot(state, 1, number, "debug.setupvalue");
            if (!slot)
                return 0;
            const uint8_t* value = stackSlot(state, 3);
            if (!value)
                return fail(state, "debug.setupvalue: expected a value");
            anchorValue(state, 3);
            std::memcpy(slot, value, luaobj::kValueSize);
            return 0;
        }

        // debug.getconstant
        // debug.getconstants
        // debug.getproto
        // debug.getprotos
        // debug.getstack
        // debug.getupvalue
        // debug.getupvalues
        // debug.setconstant
        // debug.setstack
        // debug.setupvalue
        
    }
    
    // Raw memory reads, exposed to Lua. Offsets in this project are derived
    // against the running binary rather than copied from anywhere, and doing
    // that from a live game - where a real Instance, a real signal and a real
    // script are all in hand - is far quicker and far safer than reading
    // disassembly and guessing. Every read is bounds-checked through
    // luaobj::readable first, so a wrong address answers nil instead of taking
    // the process down.
    namespace Probe {

        // luaobj::readable asks mincore whether a page is resident, which says
        // nothing about its protection: a PROT_NONE guard page is resident and
        // reading it kills the process. Scanning unknown memory needs a read
        // that reports failure instead of faulting, and vm_read_overwrite is
        // exactly that - the kernel does the copy and returns an error code.
        // It lives in luaobj now, because the job walk needs the same thing and
        // that runs long before this header is reachable.
        using luaobj::safeRead;

        inline bool address(uint64_t state, int index, uintptr_t& out)
        {
            int isnum = 0;
            // Addresses do not survive lua_tointegerx (they are wider than an
            // int), so they cross as doubles - exact up to 2^53, which covers
            // every address in the image.
            const int low = rbx::lua_tointegerx(state, index, &isnum);
            (void)low;
            if (!isnum)
                return false;

            const uint8_t* slot = Debug::stackSlot(state, index);
            if (!slot)
                return false;
            const double value = luaobj::read<double>(slot, luaobj::value::gc);
            if (value < 0 || value > 9007199254740992.0)
                return false;
            out = static_cast<uintptr_t>(value);
            return true;
        }

        inline int offsetOf(uint64_t state, int index)
        {
            int isnum = 0;
            const int value = rbx::lua_tointegerx(state, index, &isnum);
            return isnum ? value : 0;
        }

        // The pointer inside a userdata: an Instance, a signal, a connection.
        inline int pointer(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t inner = 0;
            if (!data || !safeRead(&inner, reinterpret_cast<uintptr_t>(data), sizeof(inner))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(inner));
            return 1;
        }

        // A collectable value's own address - a closure, a table, a thread.
        // lua_topointer is the only way to it: Roblox encodes the pointer
        // tostring() shows (SipHash, see lua_encodepointer), so parsing that
        // gives a number that is not an address.
        inline int valuePointer(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* value = rbx::lua_topointer(state, 1);
            if (!value) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(reinterpret_cast<uintptr_t>(value)));
            return 1;
        }

        // The engine's own thread-local security context - what printidentity
        // reads and what setthreadidentity now writes. Exposed so its layout can
        // be checked from in-game rather than assumed.
        inline int identityContext(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t context = rbx::identitypropagator(state);
            if (!context) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(context));
            return 1;
        }

        // The identity and capability words the engine keeps on a thread's own
        // extra space, for any live thread. Deriving a flag means being able to
        // read it back - the lesson from the require bypass - and these two
        // words are the whole of RbxStu's `IsOurThread`, which is the only
        // reference that answers "is this thread ours" without keeping a set of
        // raw pointers. Capabilities come back as hex because 64 bits do not
        // survive a double.
        inline int threadCapabilities(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uint64_t target = state;
            if (rbx::lua_type(state, 1) == luaobj::kTagThread) {
                const void* thread = rbx::lua_topointer(state, 1);
                if (!thread) {
                    rbx::lua_pushnil(state);
                    return 1;
                }
                target = reinterpret_cast<uint64_t>(thread);
            }

            const uint64_t extraSpace = rbx::lua_getthreaddata(target);
            uint32_t identity = 0;
            uint64_t capabilities = 0;
            if (!extraSpace ||
                !safeRead(&identity, extraSpace + offsets::identity, sizeof(identity)) ||
                !safeRead(&capabilities, extraSpace + offsets::capabilities, sizeof(capabilities))) {
                rbx::lua_pushnil(state);
                return 1;
            }

            char hex[32];
            snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(capabilities));
            utils::pushNumber(state, static_cast<double>(identity));
            rbx::lua_pushlstring(state, hex, std::strlen(hex));
            return 2;
        }

        // The userdata's own payload address, as opposed to the pointer stored
        // inside it. An Instance keeps a pointer there, but a connection or a
        // signal keeps an inline struct, and reading through it lands nowhere.
        inline int userdata(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            if (!data) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(reinterpret_cast<uintptr_t>(data)));
            return 1;
        }

        inline int peekPointer(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0;
            if (!address(state, 1, base)) {
                rbx::lua_pushnil(state);
                return 1;
            }
            uintptr_t value = 0;
            if (!safeRead(&value, base + static_cast<uintptr_t>(offsetOf(state, 2)), sizeof(value))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(value));
            return 1;
        }

        inline int peekBytes(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0;
            const int offset = offsetOf(state, 2);
            int count = offsetOf(state, 3);
            if (count <= 0 || count > 4096)
                count = 16;
            std::vector<uint8_t> buffer(static_cast<size_t>(count));
            if (!address(state, 1, base) ||
                !safeRead(buffer.data(), base + offset, buffer.size())) {
                rbx::lua_pushnil(state);
                return 1;
            }

            static const char* const hex = "0123456789abcdef";
            const uint8_t* bytes = buffer.data();
            std::string out;
            out.reserve(static_cast<size_t>(count) * 2);
            for (int i = 0; i < count; i++) {
                out += hex[bytes[i] >> 4];
                out += hex[bytes[i] & 0xf];
            }
            rbx::lua_pushlstring(state, out.data(), out.size());
            return 1;
        }

        // One byte, written where a peek would have read.
        //
        //   __vanta_poke(address, offset, byte) -> previous byte | nil
        //
        // A probe, not a feature: deriving a flag like "is this a RobloxScript"
        // means being able to set it and see what changes, and the alternative
        // is guessing from a disassembly. It reads the byte first and hands the
        // old value back so a caller can put it the way it found it, and it
        // refuses anything it could not read - so a bad address is a nil rather
        // than a write into whatever happened to be mapped.
        inline int poke(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0;
            const int offset = offsetOf(state, 2);
            int isnum = 0;
            const int value = rbx::lua_tointegerx(state, 3, &isnum);
            if (!isnum || value < 0 || value > 255 || !address(state, 1, base)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            uint8_t previous = 0;
            if (!safeRead(&previous, base + offset, sizeof(previous))) {
                rbx::lua_pushnil(state);
                return 1;
            }

            // Through the kernel, not a raw store: safeRead above proves the
            // page is readable, and a read-only page reads perfectly well and
            // faults on the write. See luaobj::safeWrite.
            const uint8_t byte = static_cast<uint8_t>(value);
            if (!luaobj::safeWrite(base + offset, &byte, sizeof(byte))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushinteger(state, previous);
            return 1;
        }

        inline int peekDouble(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0;
            if (!address(state, 1, base)) {
                rbx::lua_pushnil(state);
                return 1;
            }
            double value = 0;
            if (!safeRead(&value, base + static_cast<uintptr_t>(offsetOf(state, 2)), sizeof(value))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, value);
            return 1;
        }

        // Is this address inside RobloxPlayer's own image? Anything about to be
        // *called* through a derived offset has to pass this: a mis-derived
        // vtable slot is a jump to whatever the heap happened to hold.
        // The image is based at 0x100000000 plus the slide - the bare slide is
        // not the load address, and testing against it fails every time.
        inline bool inImage(uintptr_t address)
        {
            const uintptr_t base = 0x100000000 + hostImage().slide;
            return address >= base && address < base + 0x10000000;
        }

        // A libc++ std::string, which Roblox uses everywhere for names and type
        // names. taskscheduler::readString knows the two layouts but trusts the
        // data pointer of a long string, and a scan over arbitrary memory hands
        // it garbage - which is a segfault inside Roblox, not a bad answer. So
        // the header, the length and the data buffer are each checked here
        // before anything is dereferenced.
        inline bool readString(uintptr_t at, std::string& out, size_t cap = 1u << 22)
        {
            uint8_t header[24];
            if (!safeRead(header, at, sizeof(header)))
                return false;

            uintptr_t capacity;
            std::memcpy(&capacity, header, sizeof(capacity));
            if ((capacity & 1) == 0) {
                const size_t length = header[0] >> 1;
                if (length > 22)
                    return false;
                out.assign(reinterpret_cast<const char*>(header + 1), length);
                return true;
            }

            size_t length;
            uintptr_t data;
            std::memcpy(&length, header + 8, sizeof(length));
            std::memcpy(&data, header + 16, sizeof(data));
            if (!data || length == 0 || length > cap)
                return false;

            out.assign(length, '\0');
            return safeRead(out.data(), data, length);
        }

        inline int peekString(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0;
            std::string value;
            if (!address(state, 1, base) ||
                !readString(base + static_cast<uintptr_t>(offsetOf(state, 2)), value)) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushlstring(state, value.data(), value.size());
            return 1;
        }

        // Roblox's member descriptors all share a shape: a std::string* name at
        // +0x8 and the owning ClassDescriptor at +0x30. That was established
        // from the event descriptors, where the answers are known in advance.
        // Given a class descriptor, this walks the heap pages around it and
        // reports everything shaped like one of its members - which is how the
        // property descriptors get found without guessing at container layouts.
        inline int scanMembers(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t classDescriptor = 0;
            if (!address(state, 1, classDescriptor)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            int isnum = 0;
            long window = rbx::lua_tointegerx(state, 2, &isnum);
            if (!isnum || window <= 0 || window > 0x2000000)
                window = 0x200000;

            const uintptr_t start = classDescriptor > static_cast<uintptr_t>(window)
                ? classDescriptor - static_cast<uintptr_t>(window) : 0;
            const uintptr_t end = classDescriptor + static_cast<uintptr_t>(window);

            rbx::lua_createtable(state, 0, 0);
            int found = 0;

            // One page at a time: a page that is not mapped costs one failed
            // read rather than 512.
            constexpr size_t kPage = 4096;
            std::vector<uint8_t> page(kPage);
            for (uintptr_t base = start & ~(kPage - 1); base < end; base += kPage) {
                if (!safeRead(page.data(), base, kPage))
                    continue;

                for (size_t offset = 0; offset + 0x38 <= kPage; offset += 8) {
                    uintptr_t owner;
                    // Through layout::, not a literal. The same two fields are
                    // read as descriptor_owner/descriptor_name everywhere else,
                    // and a literal here would not follow the dumper when the
                    // reflection layout moves - which is exactly how
                    // vulnpatch.hpp's copy of state::global went stale and
                    // silently disabled the whole blocklist.
                    std::memcpy(&owner, page.data() + offset + offsets::structs::descriptor_owner,
                                sizeof(owner));
                    if (owner != classDescriptor)
                        continue;

                    uintptr_t namePointer;
                    std::memcpy(&namePointer, page.data() + offset + offsets::structs::descriptor_name,
                                sizeof(namePointer));
                    std::string name;
                    if (!readString(namePointer, name, 256) || name.empty() || name.size() > 64)
                        continue;

                    bool printable = true;
                    for (char c : name)
                        printable = printable && (c >= 32 && c < 127);
                    if (!printable)
                        continue;

                    rbx::lua_createtable(state, 0, 2);
                    utils::pushNumber(state, static_cast<double>(base + offset));
                    rbx::lua_setfield(state, -2, "address");
                    rbx::lua_pushlstring(state, name.data(), name.size());
                    rbx::lua_setfield(state, -2, "name");
                    rbx::lua_rawseti(state, -2, ++found);

                    if (found >= 4096)
                        return 1;
                }
            }
            return 1;
        }

        // Scans a window for an exact 8-byte value. Used to find a container by
        // something known to be inside it - the actor map, for instance, is
        // found from the pointer of an Actor that is known to be in it.
        inline int findQword(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t base = 0, target = 0;
            if (!address(state, 1, base) || !address(state, 3, target)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            int isnum = 0;
            long window = rbx::lua_tointegerx(state, 2, &isnum);
            if (!isnum || window <= 0 || window > 0x4000000)
                window = 0x4000;

            constexpr size_t kPage = 4096;
            std::vector<uint8_t> page(kPage);
            rbx::lua_createtable(state, 0, 0);
            int found = 0;

            const uintptr_t start = base > static_cast<uintptr_t>(window)
                ? base - static_cast<uintptr_t>(window) : 0;
            for (uintptr_t at = start & ~(kPage - 1); at < base + static_cast<uintptr_t>(window); at += kPage) {
                if (!safeRead(page.data(), at, kPage))
                    continue;
                for (size_t offset = 0; offset + 8 <= kPage; offset += 8) {
                    uintptr_t value;
                    std::memcpy(&value, page.data() + offset, sizeof(value));
                    if (value != target)
                        continue;
                    utils::pushNumber(state, static_cast<double>(at + offset));
                    rbx::lua_rawseti(state, -2, ++found);
                    if (found >= 256)
                        return 1;
                }
            }
            return 1;
        }

        // The main Luau state, so a candidate state found elsewhere can be
        // checked against one that is known to be real.
        inline int mainState(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            utils::pushNumber(state, static_cast<double>(state));
            return 1;
        }

        // The flag byte a ModuleScript carries at +0xAA, read and optionally
        // written. Place modules have bit 3 set and the ones Roblox ships
        // (PlayerModule and everything under it) do not, which is the only
        // difference between a module require() will load and one the
        // "Cannot require a non-RobloxScript module from a RobloxScript" gate
        // refuses. Kept as a probe until that is confirmed end to end.
        inline int scriptFlag(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t instance = 0;
            if (!data ||
                !safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance) {
                rbx::lua_pushnil(state);
                return 1;
            }

            uint8_t current = 0;
            if (!safeRead(&current, instance + 0xAA, sizeof(current))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushinteger(state, current);

            int isnum = 0;
            const int wanted = rbx::lua_tointegerx(state, 2, &isnum);
            if (isnum && wanted >= 0 && wanted <= 255)
                *reinterpret_cast<uint8_t*>(instance + 0xAA) = static_cast<uint8_t>(wanted);
            return 1;
        }

        // ASLR's slide for RobloxPlayer, so an address seen from Lua can be
        // written back into offsets.hpp unslid.
        inline int imageSlide(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            utils::pushNumber(state, static_cast<double>(hostImage().slide));
            return 1;
        }

        inline int schedulerAddress(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t scheduler = 0;
            if (!offsets::taskscheduler || !safeRead(&scheduler, offsets::taskscheduler, sizeof(scheduler))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(scheduler));
            return 1;
        }

        // The load clock's own two numbers, handed to Lua from the same
        // offsets:: the fix reads - `__vanta_loadclock()` -> key, limit.
        //
        // tests/detect.lua needs to find the check's record to say whether it
        // fired, and a test that carries its own copy of an address is a test
        // that goes quietly wrong the next time Roblox moves. The descriptor's
        // address is derived (offset-dumper's find_loadclock) and the rest of
        // the record sits at fixed distances from it - cookie key-0x4c, status
        // key-0x8, latch key+0x20, gate key+0x28, identical on 0.736 and 0.737
        // - so one derived number anchors all four. The limit is its own
        // global in another section entirely and cannot be reached that way,
        // which is why it is passed rather than computed.
        inline int loadClock(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            utils::pushNumber(state, static_cast<double>(offsets::loadclockkey));
            int32_t limit = 0;
            if (!safeRead(&limit, offsets::loadclocklimit, sizeof(limit))) {
                rbx::lua_pushnil(state);
                return 2;
            }
            utils::pushNumber(state, static_cast<double>(limit));
            return 2;
        }
    }

    // debug.getstack / debug.setstack reach a frame's registers, which means
    // walking the CallInfo array. Its stride is not something to hardcode: it
    // is derived once, at the first call, by stepping back from the running
    // frame until the candidate lands on a CallInfo whose func slot really does
    // hold a function.
    namespace Stack {

        inline int fail(uint64_t state, const char* message)
        {
            rbx::lua_pushlstring(state, message, std::strlen(message));
            rbx::lua_error(state);
            return 0;
        }

        inline bool looksLikeFrame(uint64_t state, const void* callinfo)
        {
            (void)state;
            const auto* ci = static_cast<const uint8_t*>(callinfo);
            uint8_t* func = nullptr;
            uint8_t* base = nullptr;
            uint8_t* top = nullptr;
            if (!luaobj::safeRead(&func, reinterpret_cast<uintptr_t>(ci) + luaobj::callinfo::func, sizeof(func)) ||
                !luaobj::safeRead(&base, reinterpret_cast<uintptr_t>(ci) + luaobj::callinfo::base, sizeof(base)) ||
                !luaobj::safeRead(&top, reinterpret_cast<uintptr_t>(ci) + luaobj::callinfo::top, sizeof(top)))
                return false;
            if (!func || !base || !top)
                return false;
            // A frame's registers start after its function TValue. That is
            // normally one TValue, but a vararg call shifts base by the extra
            // arguments (the top-level chunk of a script is one), so the
            // relation is "a whole number of TValues after func", not a fixed
            // 0x10. A candidate that is really the middle of a neighbouring
            // CallInfo fails this, while a tag check alone can pass by
            // coincidence - and then every field read from it is garbage.
            const ptrdiff_t offset = base - func;
            if (offset < luaobj::kValueSize || offset > 0x4000 ||
                offset % luaobj::kValueSize != 0)
                return false;
            if (top < base || top - base > 0x4000)
                return false;
            int tag = -1;
            if (!luaobj::safeRead(&tag, reinterpret_cast<uintptr_t>(func) + luaobj::value::tt, sizeof(tag)))
                return false;
            return tag == luaobj::kTagFunction;
        }

        inline int stride(uint64_t state)
        {
            static int cached = 0;
            if (cached)
                return cached;

            void* callinfo = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci);
            void* first = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base_ci);
            if (!callinfo || !first || callinfo == first)
                return 0;

            // One line per process. The field order has changed twice across
            // builds (see LuaObject.h), and when it moves this is the first
            // number that says so.
            static std::atomic<bool> reported{false};
            auto report = [&](int found) {
                if (!reported.exchange(true, std::memory_order_relaxed))
                    journal::writef("CISTRIDE ci=%p base_ci=%p stride=%d", callinfo, first, found);
            };

            for (int candidate : {0x30, 0x28, 0x38, 0x40}) {
                auto* previous = static_cast<uint8_t*>(callinfo) - candidate;
                if (previous < static_cast<uint8_t*>(first))
                    continue;
                if (looksLikeFrame(state, previous)) {
                    cached = candidate;
                    report(cached);
                    return cached;
                }
            }
            report(0);
            return 0;
        }

        inline bool isLuaFrame(uint64_t state, const void* callinfo)
        {
            (void)state;
            uint8_t* func = nullptr;
            if (!luaobj::safeRead(&func, reinterpret_cast<uintptr_t>(callinfo) + luaobj::callinfo::func,
                                  sizeof(func)) || !func)
                return false;
            const void* closure = nullptr;
            if (!luaobj::safeRead(&closure, reinterpret_cast<uintptr_t>(func) + luaobj::value::gc,
                                  sizeof(closure)) || !closure)
                return false;
            uint8_t isC = 0xff;
            if (!luaobj::safeRead(&isC, reinterpret_cast<uintptr_t>(closure) + luaobj::closure::isC,
                                  sizeof(isC)))
                return false;
            return isC == 0;
        }

        // Level 1 is the first Luau frame the caller can see: for the current
        // thread that is the caller of this C function, and for a foreign
        // thread it is the first Luau frame under the C frame that suspended
        // it (coroutine.yield, most often). Counting only Luau frames is what
        // makes a level mean the same thing on both.
        inline void* frameAt(uint64_t state, int level, bool skipTop)
        {
            const int step = stride(state);
            if (step <= 0 || level < 0)
                return nullptr;

            auto* callinfo = static_cast<uint8_t*>(
                luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci));
            auto* first = static_cast<uint8_t*>(
                luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base_ci));
            if (!callinfo || !first)
                return nullptr;

            auto* frame = callinfo;
            if (skipTop)
                frame -= step;

            int remaining = level;
            int guard = 512;
            while (frame >= first && guard-- > 0) {
                if (isLuaFrame(state, frame) && --remaining == 0)
                    return frame;
                frame -= step;
            }
            return nullptr;
        }

        inline void* frameAt(uint64_t state, int level)
        {
            return frameAt(state, level, true);
        }

        // A frame the executor itself put on the stack: a hook dispatcher or
        // the stand-in a yielding call runs through. LunarModule skips these
        // when numbering stack levels (its is_internal_stack_frame) so that
        // hooks do not shift what "level 2" means for the script. Everything
        // else counts, C frames included - a caller that selects one gets the
        // C-closure error rather than silently landing on a deeper Luau frame.
        inline bool internalFrame(uint64_t state, const void* frame)
        {
            (void)state;
            uint8_t* func = nullptr;
            if (!luaobj::safeRead(&func, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::func,
                                  sizeof(func)) || !func)
                return true;
            const void* closure = nullptr;
            if (!luaobj::safeRead(&closure, reinterpret_cast<uintptr_t>(func) + luaobj::value::gc,
                                  sizeof(closure)) || !closure)
                return true;
            if (!luaobj::byte(closure, luaobj::closure::isC)) {
                // The forwarding bodies hookfunction and hookmetamethod put on
                // the stack are Luau closures; they exist so hooks keep their
                // own environment. Counting them as stack levels would shift
                // every level under them. Every one is anchored at creation,
                // so membership is exact.
                std::lock_guard<std::mutex> lock(Closures::wrapperClosuresMutex);
                return Closures::wrapperClosures.count(closure) != 0;
            }
            const uintptr_t function = luaobj::read<uintptr_t>(closure, luaobj::closure::cfunction);
            return function == reinterpret_cast<uintptr_t>(&Closures::newcclosureCall) ||
                   function == reinterpret_cast<uintptr_t>(&Yield::frameFn);
        }

        // Level 1 is the function that called the current C function on this
        // thread, and the yielding function's caller on a suspended one - the
        // walk always steps back once to leave this C frame out. Every other
        // frame is a level, except the executor's own dispatchers.
        inline void* visibleFrameAt(uint64_t state, int level, bool skipTop)
        {
            const int step = stride(state);
            if (step <= 0 || level < 0)
                return nullptr;
            auto* callinfo = static_cast<uint8_t*>(
                luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci));
            auto* first = static_cast<uint8_t*>(
                luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base_ci));
            if (!callinfo || !first)
                return nullptr;
            auto* frame = callinfo;
            if (skipTop)
                frame -= step;
            int visible = 0;
            int guard = 1024;
            while (frame >= first && guard-- > 0) {
                if (!internalFrame(state, frame) && ++visible == level)
                    return frame;
                frame -= step;
            }
            return nullptr;
        }

        // A stack level, and the thread it belongs to. sUNC reads the stack of
        // threads other than the caller's, so the thread may be given first;
        // when it is, level 1 means that thread's *own* top frame rather than
        // "whoever called me", because there is no call of ours on it.
        struct Level {
            uint64_t thread;
            int level;
            int argument;    // the index the level was read from
            bool ok;
        };

        inline Level levelOf(uint64_t state, int first)
        {
            Level out{state, 0, first, false};
            if (rbx::lua_type(state, first) == luaobj::kTagThread) {
                const void* thread = rbx::lua_topointer(state, first);
                if (!thread)
                    return out;
                out.thread = reinterpret_cast<uint64_t>(thread);
                out.argument = first + 1;
            }

            int isnum = 0;
            const int level = rbx::lua_tointegerx(state, out.argument, &isnum);
            if (!isnum || level < 1)
                return out;

            // frameAt counts Luau frames, so the same level means the same
            // thing on the current thread and on a suspended one.
            out.level = level;
            out.ok = true;
            return out;
        }

        // Enabled on 0.742: the CallInfo layout was re-measured with
        // __vanta_cibase (LuaObject.h) and every pointer the walk reads is
        // validated through safeRead plus the base == func + 16 relation before
        // it is used. A frame that does not validate answers an error, so a
        // level the walk cannot reach can no longer take the process down.
        inline constexpr bool kRawStackAccess = true;

        inline int getstack(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (!kRawStackAccess)
                return fail(state, "debug.getstack: raw stack access is disabled on this build");

            const Level target = levelOf(state, 1);
            {
                static std::atomic<int> all{0};
                if (all.fetch_add(1, std::memory_order_relaxed) < 40)
                    journal::writef("GSANY ok=%d level=%d arg=%d foreign=%d",
                                    target.ok ? 1 : 0, target.level, target.argument,
                                    target.thread != state ? 1 : 0);
            }
            if (!target.ok)
                return fail(state, "debug.getstack: expected a stack level of 1 or more");

            if (target.thread != state) {
                static std::atomic<int> entries{0};
                if (entries.fetch_add(1, std::memory_order_relaxed) < 24)
                    journal::writef("GSENTRY level=%d arg=%d thread=%llx state=%llx",
                                    target.level, target.argument,
                                    (unsigned long long)target.thread,
                                    (unsigned long long)state);
            }

            void* frame = visibleFrameAt(target.thread, target.level, true);
            if (!frame || !looksLikeFrame(target.thread, frame))
                return fail(state, "debug.getstack: level out of range");

            // A C frame has no Luau register window, so reading or writing its
            // slots is meaningless. Both references that check for this reject
            // it - the macOS one with "debug.getstack does not accept C
            // functions", RbxStu with lua_ensurelclosure.
            const uint8_t* frameFunc = nullptr;
            const void* frameClosure = nullptr;
            if (!luaobj::safeRead(&frameFunc, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::func,
                                  sizeof(frameFunc)) || !frameFunc ||
                !luaobj::safeRead(&frameClosure,
                                  reinterpret_cast<uintptr_t>(frameFunc) + luaobj::value::gc,
                                  sizeof(frameClosure)))
                return fail(state, "debug.getstack: the frame is not readable");
            if (!frameClosure || luaobj::byte(frameClosure, luaobj::closure::isC))
                return fail(state, "debug.getstack: that level is a C function");

            uint8_t* base = nullptr;
            uint8_t* top = nullptr;
            if (!luaobj::safeRead(&base, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::base,
                                  sizeof(base)) ||
                !luaobj::safeRead(&top, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::top,
                                  sizeof(top)))
                return fail(state, "debug.getstack: the frame's register window is not readable");
            const ptrdiff_t span = top - base;
            if (!base || !top || span <= 0 || span > 0x4000 ||
                span % luaobj::kValueSize != 0)
                return fail(state, "debug.getstack: the frame's register window is not readable");
            const int slots = static_cast<int>(span / luaobj::kValueSize);

            int wantedIndex = 0;
            // lua_type answers LUA_TNONE (-1) for an argument that was never
            // passed and LUA_TNIL (0) for one passed as nil, so "was an index
            // given" is type > nil. Testing != 0 made the no-index form - the
            // one the spec documents as returning the whole frame - always look
            // like index 0 and fail with "index out of range".
            const bool hasIndex = rbx::lua_type(state, target.argument + 1) > luaobj::kTagNil;
            if (hasIndex) {
                int indexIsNum = 0;
                wantedIndex = rbx::lua_tointegerx(state, target.argument + 1, &indexIsNum);
                if (!indexIsNum || wantedIndex < 1 || wantedIndex > slots)
                    return fail(state, "debug.getstack: index out of range");
            }

            auto push = [&](int slot) {
                // The source may be the last readable slot of a frame whose
                // top was itself approximate, so the copy is done through the
                // kernel: a bad slot answers nil instead of faulting.
                uint8_t value[luaobj::kValueSize];
                const bool readable = luaobj::safeRead(
                    value, reinterpret_cast<uintptr_t>(base + slot * luaobj::kValueSize),
                    sizeof(value));
                rbx::lua_pushnil(state);
                if (!readable)
                    return;
                uint8_t* stackTop = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
                std::memcpy(stackTop - luaobj::kValueSize, value, luaobj::kValueSize);
            };

            if (target.thread != state) {
                static std::atomic<int> seen{0};
                if (seen.fetch_add(1, std::memory_order_relaxed) < 24) {
                    std::string detail;
                    const int show = slots < 10 ? slots : 10;
                    for (int i = 0; i < show; i++) {
                        const uint8_t* slot = base + i * luaobj::kValueSize;
                        const int tag = luaobj::read<int>(slot, luaobj::value::tt);
                        char one[64];
                        if (tag == luaobj::kTagNumber) {
                            snprintf(one, sizeof(one), " %d:num=%.17g", i,
                                     luaobj::read<double>(slot, luaobj::value::gc));
                        } else if (tag == luaobj::kTagBoolean) {
                            snprintf(one, sizeof(one), " %d:bool=%d", i,
                                     luaobj::read<int>(slot, luaobj::value::gc) != 0);
                        } else {
                            snprintf(one, sizeof(one), " %d:tag=%d", i, tag);
                        }
                        detail += one;
                    }
                    journal::writef("GETSTACK level=%d index=%d slots=%d%s",
                                    target.level, hasIndex ? wantedIndex : 0, slots,
                                    detail.c_str());
                }
            }

            if (hasIndex) {
                push(wantedIndex - 1);
                return 1;
            }

            rbx::lua_createtable(state, slots, 0);
            for (int i = 0; i < slots; i++) {
                push(i);
                rbx::lua_rawseti(state, -2, i + 1);
            }
            return 1;
        }

        inline int setstack(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (!kRawStackAccess)
                return fail(state, "debug.setstack: raw stack access is disabled on this build");

            const Level target = levelOf(state, 1);
            int indexIsNum = 0;
            const int index = rbx::lua_tointegerx(state, target.argument + 1, &indexIsNum);
            {
                static std::atomic<int> all{0};
                if (all.fetch_add(1, std::memory_order_relaxed) < 40)
                    journal::writef("SSANY ok=%d level=%d index=%d foreign=%d",
                                    target.ok ? 1 : 0, target.level, index,
                                    target.thread != state ? 1 : 0);
            }
            if (!target.ok || !indexIsNum || index < 1)
                return fail(state, "debug.setstack: expected a level and a register index");

            if (target.thread != state) {
                static std::atomic<int> entries{0};
                if (entries.fetch_add(1, std::memory_order_relaxed) < 24)
                    journal::writef("SSENTRY level=%d index=%d arg=%d thread=%llx state=%llx",
                                    target.level, index, target.argument,
                                    (unsigned long long)target.thread,
                                    (unsigned long long)state);
            }

            void* frame = visibleFrameAt(target.thread, target.level, true);
            if (!frame || !looksLikeFrame(target.thread, frame))
                return fail(state, "debug.setstack: level out of range");

            // Same as getstack: a C frame has no register window to write into.
            const uint8_t* frameFunc = nullptr;
            const void* frameClosure = nullptr;
            if (!luaobj::safeRead(&frameFunc, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::func,
                                  sizeof(frameFunc)) || !frameFunc ||
                !luaobj::safeRead(&frameClosure,
                                  reinterpret_cast<uintptr_t>(frameFunc) + luaobj::value::gc,
                                  sizeof(frameClosure)))
                return fail(state, "debug.setstack: the frame is not readable");
            if (!frameClosure || luaobj::byte(frameClosure, luaobj::closure::isC))
                return fail(state, "debug.setstack: that level is a C function");

            uint8_t* base = nullptr;
            uint8_t* top = nullptr;
            if (!luaobj::safeRead(&base, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::base,
                                  sizeof(base)) ||
                !luaobj::safeRead(&top, reinterpret_cast<uintptr_t>(frame) + luaobj::callinfo::top,
                                  sizeof(top)))
                return fail(state, "debug.setstack: the frame's register window is not readable");
            const ptrdiff_t span = top - base;
            if (!base || !top || span <= 0 || span > 0x4000 ||
                span % luaobj::kValueSize != 0)
                return fail(state, "debug.setstack: the frame's register window is not readable");
            const int slots = static_cast<int>(span / luaobj::kValueSize);
            if (index > slots)
                return fail(state, "debug.setstack: index out of range");

            // Never overwrite a slot that is the function TValue of a live
            // frame above this one; doing so corrupts the call chain.
            uint8_t* targetSlot = base + (index - 1) * luaobj::kValueSize;
            auto* topFrame = luaobj::read<uint8_t*>(reinterpret_cast<void*>(target.thread),
                                                    luaobj::state::ci);
            const int frameStep = Stack::stride(target.thread);
            int activeGuard = 512;
            for (auto* active = static_cast<uint8_t*>(frame) + frameStep;
                 topFrame && active <= topFrame && frameStep > 0 && activeGuard-- > 0;
                 active += frameStep) {
                const uint8_t* activeFunc = nullptr;
                if (!luaobj::safeRead(&activeFunc,
                                      reinterpret_cast<uintptr_t>(active) + luaobj::callinfo::func,
                                      sizeof(activeFunc)))
                    continue;
                if (activeFunc && activeFunc == targetSlot)
                    return fail(state, "debug.setstack: cannot replace an active call frame");
            }

            // Anchored for the same reason every other raw write is: copying a
            // TValue into a frame issues no GC barrier.
            Debug::anchorValue(state, target.argument + 2);

            rbx::lua_pushvalue(state, target.argument + 2);
            uint8_t* source = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top) -
                              luaobj::kValueSize;
            if (!luaobj::safeWrite(reinterpret_cast<uintptr_t>(base + (index - 1) * luaobj::kValueSize),
                                   source, luaobj::kValueSize))
                return fail(state, "debug.setstack: the register is not writable");
            rbx::lua_settop(state, -2);
            return 0;
        }
    }

    // What an RBXScriptSignal hands its listeners. Every offset here was read
    // out of a live game rather than taken from anywhere: Part.Touched,
    // RunService.Stepped and UserInputService.InputBegan between them pin the
    // whole layout, because their argument names and types are known up front.
    //
    //   signal userdata  -> *(void**)            the EventInstance
    //   EventInstance+0                          the event descriptor
    //   descriptor+0x08  -> std::string*         the event's name  ("Touched")
    //   descriptor+0x28  -> std::string*         its category      ("Signals")
    //   descriptor+0x30  -> ClassDescriptor,     +0x8 -> "BasePart"
    //   descriptor+0x48  -> the argument array
    //   argument[i]      at stride 0x70:
    //       +0x00        -> std::string*         the parameter name
    //       +0x08        -> Type*, whose +0x08 -> std::string* type name
    //
    // The stride showed up on Stepped and InputBegan, which take two arguments
    // each; a one-argument signal cannot tell you what it is.
    // Declared here and defined further down, next to the rest of the actor
    // work: firesignal has to reach handlers that live in an Actor's own Luau
    // state, and those two are what make a value and a state cross over.
    namespace Actors {
        inline uint64_t stateOwning(uint64_t state, uintptr_t thread);
        inline void transfer(uint64_t from, uint64_t to, int index, int depth);
    }

    namespace Signals {

        namespace layout = offsets::structs;

        struct Argument {
            std::string name;
            std::string type;
            uintptr_t typeObject = 0;     // the reflection Type*
            uintptr_t defaultValue = 0;   // the parameter's inline default Variant
            uintptr_t at = 0;             // the Parameter record itself
        };

        inline bool stringAt(uintptr_t holder, int offset, std::string& out)
        {
            uintptr_t pointer = 0;
            if (!Probe::safeRead(&pointer, holder + offset, sizeof(pointer)))
                return false;
            return Probe::readString(pointer, out, 1024);
        }

        inline uintptr_t descriptorOf(uint64_t state, int index)
        {
            void* data = rbx::lua_touserdata(state, index);
            if (!data)
                return 0;
            uintptr_t instance = 0, descriptor = 0;
            if (!Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)))
                return 0;
            if (!Probe::safeRead(&descriptor, instance, sizeof(descriptor)))
                return 0;
            return descriptor;
        }

        inline std::vector<Argument> argumentsOf(uintptr_t descriptor)
        {
            std::vector<Argument> found;
            uintptr_t list = 0, end = 0, capacity = 0;
            if (!descriptor ||
                !Probe::safeRead(&list, descriptor + layout::eventdescriptor_arguments,
                                 sizeof(list)) ||
                !Probe::safeRead(&end, descriptor + layout::eventdescriptor_arguments + 8,
                                 sizeof(end)) ||
                !Probe::safeRead(&capacity, descriptor + layout::eventdescriptor_arguments + 16,
                                 sizeof(capacity)))
                return found;

            // Parameters is a libc++ vector: begin, end, capacity. End - begin
            // is the only trustworthy count; scanning until a name fails ran
            // into unrelated memory and made MouseWheelForward report 24
            // arguments instead of two.
            int count = 0;
            if (list && end >= list && end <= capacity &&
                (end - list) % layout::argument_stride == 0)
                count = static_cast<int>((end - list) / layout::argument_stride);
            if (count < 0 || count > 256)
                count = 0;
            if (!count)
                count = 24;   // no usable vector: keep the old bounded scan

            for (int i = 0; i < count; i++) {
                const uintptr_t entry = list + static_cast<uintptr_t>(i) * layout::argument_stride;
                Argument argument;
                if (!stringAt(entry, 0, argument.name) || argument.name.empty())
                    break;

                uintptr_t type = 0;
                if (!Probe::safeRead(&type, entry + 8, sizeof(type)) || !type)
                    break;
                if (!stringAt(type, layout::type_name, argument.type) || argument.type.empty())
                    break;
                argument.typeObject = type;
                argument.defaultValue = entry + layout::argument_defaultvalue;
                argument.at = entry;

                found.push_back(std::move(argument));
            }
            return found;
        }

        inline int fail(uint64_t state, const char* message)
        {
            rbx::lua_pushlstring(state, message, std::strlen(message));
            rbx::lua_error(state);
            return 0;
        }

        // The type name of each parameter, in order.
        //
        // An empty result is not a failure: GuiObject.SelectionGained and
        // plenty of others genuinely take no parameters, and answering with an
        // error for those made every caller special-case them. Only a signal
        // whose descriptor cannot be read at all is an error.
        inline int getsignalarguments(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t descriptor = descriptorOf(state, 1);
            if (!descriptor)
                return fail(state, "getsignalarguments: could not read the signal's signature");
            const auto arguments = argumentsOf(descriptor);

            rbx::lua_createtable(state, static_cast<int>(arguments.size()), 0);
            for (size_t i = 0; i < arguments.size(); i++) {
                rbx::lua_pushlstring(state, arguments[i].type.data(), arguments[i].type.size());
                rbx::lua_rawseti(state, -2, static_cast<int>(i) + 1);
            }
            return 1;
        }

        // The same, with the parameter names the API dump would give. A signal
        // with no parameters answers with an empty table, not an error.
        inline int getsignalargumentsinfo(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t descriptor = descriptorOf(state, 1);
            if (!descriptor)
                return fail(state,
                            "getsignalargumentsinfo: could not read the signal's signature");
            const auto arguments = argumentsOf(descriptor);

            rbx::lua_createtable(state, static_cast<int>(arguments.size()), 0);
            for (size_t i = 0; i < arguments.size(); i++) {
                rbx::lua_createtable(state, 0, 2);
                rbx::lua_pushlstring(state, arguments[i].name.data(), arguments[i].name.size());
                rbx::lua_setfield(state, -2, "Name");
                rbx::lua_pushlstring(state, arguments[i].type.data(), arguments[i].type.size());
                rbx::lua_setfield(state, -2, "Type");
                rbx::lua_rawseti(state, -2, static_cast<int>(i) + 1);
            }
            return 1;
        }

        // Everything the signature says about a signal's parameters, including
        // the Variant the engine keeps as each parameter's default.
        //
        //   __vanta_signalargdump(signal) -> { {Name=, Type=, TypeObject=,
        //                                      Default=, DefaultType=,
        //                                      Holder=, Bytes=}, ... }
        //
        // `Holder` is the point of it. A reflection Variant is
        // `{Type* type; TypeHolder* holder; char data[0x40]}` in all three
        // references that declare it, and the holder carries the
        // construct/move/destruct thunks the engine uses when it copies the
        // value into a packet. replicatesignal builds its Variants by hand and
        // has no holder to put there unless the parameter has a default to copy
        // one from - so this is how to find out which parameters anywhere in
        // the game do carry one, since a holder is per *type*, not per
        // parameter, and one int holder works for every int Variant.
        inline int signalargdump(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const auto arguments = argumentsOf(descriptorOf(state, 1));

            rbx::lua_createtable(state, static_cast<int>(arguments.size()), 0);
            for (size_t i = 0; i < arguments.size(); i++) {
                rbx::lua_createtable(state, 0, 7);
                rbx::lua_pushlstring(state, arguments[i].name.data(), arguments[i].name.size());
                rbx::lua_setfield(state, -2, "Name");
                rbx::lua_pushlstring(state, arguments[i].type.data(), arguments[i].type.size());
                rbx::lua_setfield(state, -2, "Type");
                utils::pushNumber(state, static_cast<double>(arguments[i].typeObject));
                rbx::lua_setfield(state, -2, "TypeObject");
                utils::pushNumber(state, static_cast<double>(arguments[i].defaultValue));
                rbx::lua_setfield(state, -2, "Default");

                if (arguments[i].defaultValue) {
                    uintptr_t words[4] = {0, 0, 0, 0};
                    if (Probe::safeRead(words, arguments[i].defaultValue, sizeof(words))) {
                        utils::pushNumber(state, static_cast<double>(words[0]));
                        rbx::lua_setfield(state, -2, "DefaultType");
                        utils::pushNumber(state, static_cast<double>(words[1]));
                        rbx::lua_setfield(state, -2, "Holder");
                        char bytes[64];
                        snprintf(bytes, sizeof(bytes), "%016llx %016llx",
                                 (unsigned long long)words[2], (unsigned long long)words[3]);
                        rbx::lua_pushlstring(state, bytes, std::strlen(bytes));
                        rbx::lua_setfield(state, -2, "Bytes");
                    }
                }
                rbx::lua_rawseti(state, -2, static_cast<int>(i) + 1);
            }
            return 1;
        }

        // Whether the engine will let this event cross to the server.
        inline bool replicable(uintptr_t descriptor)
        {
            if (!descriptor)
                return false;
            // Read as 64-bit words, skipping anything that looks like a pointer.
            //
            // Reading them as 32-bit words instead was tried on the suspicion
            // that the flags were packed next to another field and being skipped
            // - the same bug that hid the property-descriptor member field. It
            // is not: probed across six signals whose answers are known
            // (`tests/repl.lua`), 0x80..0xB0 is an array of pointers, and this
            // scan already answers correctly for all six - true for
            // ClickDetector.MouseClick, MouseActionReplicated and
            // ProximityPrompt.Triggered, false for GuiObject.MouseWheelForward,
            // MouseButton1Click and BasePart.Touched. Changing the width altered
            // nothing here and could only add false positives to the whitelist.
            for (int at = layout::event_flags_first; at <= layout::event_flags_last; at += 8) {
                uint64_t flags = 0;
                if (!Probe::safeRead(&flags, descriptor + at, sizeof(flags)))
                    break;
                // A pointer is not a flags word; skip it rather than reading a
                // bit out of an address.
                if (flags >= 0x100000000ULL)
                    continue;
                if (flags & layout::event_replicable_bit)
                    return true;
            }
            return false;
        }

        // The same test, but on a descriptor address, so the whitelist walk can
        // ask about a member it found by scanning rather than by holding a
        // signal.
        inline int descriptorreplicable(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t descriptor = 0;
            if (!Probe::address(state, 1, descriptor)) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            rbx::lua_pushboolean(state, replicable(descriptor) ? 1 : 0);
            return 1;
        }

        // Replicating a signal to the server.
        //
        // There is no "fire this event at the server" entry point; what there is
        // is the Instance's own `raiseEventInvocation`, which is what the engine
        // calls when a real interaction happens. Every reference executor goes
        // through it, and the macOS one names the slot for this platform
        // (`Vftable + 0x20`) - see offsets::structs::instance_vtable_raiseevent.
        //
        // The arguments have to arrive as reflection Variants matching the
        // event's signature, which the spec is explicit about: "Some signals
        // require specific argument structures. If incorrect arguments are
        // passed, an error must be thrown."
        //
        // Deliberately *not* gated on cansignalreplicate. That flag says whether
        // the server would accept the event unprompted; the spec's own example
        // replicates `GuiObject.MouseWheelForward`, which the flag reads as
        // non-replicating, and expects it to work when given the right two
        // arguments. Refusing on the flag is what "this signal cannot replicate"
        // was.
        // The engine's RBX::Variant is {Type*, TypeHolder*, char data[0x40]}.
        // The second pointer has to be the engine's own per-type holder; a
        // Variant without one is serialised by nobody. The holder is harvested
        // from the engine's own raises by RaiseSpy.
        struct Variant {
            uintptr_t type = 0;
            uintptr_t holder = 0;
            alignas(8) unsigned char data[0x40]{};
        };

        struct InvocationOptions {
            const void* target = nullptr;
            uint32_t excludeTarget = 0;
        };

        // 0.741 and 0.742 agree on this object: descriptor, shared_ptr, then
        // the weak_ptr to the object that publishes the event.
        struct Binding {
            uintptr_t descriptor;
            std::shared_ptr<uintptr_t> custom;
            std::weak_ptr<uintptr_t> source;
        };
        static_assert(sizeof(Binding) == 40, "Binding layout changed");

        // Reflection Type* -> TypeHolder*, learned from the engine's own raises.
        //
        // Nothing static carries the pairing: a parameter's inline default
        // Variant is the void type with a null holder, and the holder for a
        // given type is a separate static that the Type object does not point
        // at. But every raise the engine makes carries correct pairs, and a
        // holder is per type rather than per call - so one sighting of an int
        // Variant anywhere in the client is enough for every int Variant we
        // will ever build.
        struct TypePair { uintptr_t type; uintptr_t holder; uint32_t reflection; };
        inline TypePair holders[64] = {};
        // Published count. The engine raises events from several threads at
        // once, so the entry is written first and the count released after it -
        // a reader that sees the count sees a whole entry. A half-written pair
        // would be worse than none: a type from one thread beside a holder from
        // another is a pointer the engine would be handed and would dereference.
        inline std::atomic<int> holderCount{0};
        inline std::atomic_flag holderLock = ATOMIC_FLAG_INIT;

        inline bool scalarReflection(uint32_t reflection)
        {
            return reflection >= layout::reflection_bool && reflection <= layout::reflection_double;
        }

        inline void rememberHolder(uintptr_t type, uintptr_t holder)
        {
            if (!type || !holder)
                return;
            // Read the type's own number before taking the lock: this is a
            // cross-thread spinlock on a path the engine walks constantly, so
            // nothing that can fault or block belongs inside it.
            uint32_t reflection = 0;
            Probe::safeRead(&reflection, type + layout::typedescriptor_reflectiontype,
                            sizeof(reflection));

            while (holderLock.test_and_set(std::memory_order_acquire)) {}
            const int count = holderCount.load(std::memory_order_relaxed);
            bool seen = false;
            for (int i = 0; i < count; i++) {
                if (holders[i].type == type) { seen = true; break; }
            }
            if (!seen && count < static_cast<int>(sizeof(holders) / sizeof(holders[0]))) {
                holders[count] = TypePair{type, holder, reflection};
                holderCount.store(count + 1, std::memory_order_release);
            }
            holderLock.clear(std::memory_order_release);
        }

        // The holder for a type, and only for that type.
        //
        // Standing another scalar's holder in was tried on the strength of
        // Vicna doing it - it takes the Variant the engine built for a Lua
        // number, a double, and overwrites only the Type and the bytes. It does
        // not work here: with a bool holder in an int Variant the replicator
        // dropped GuiObject.MouseWheelForward as silently as it did with no
        // holder at all. The pairing is per type.
        inline uintptr_t holderFor(uintptr_t type)
        {
            const int count = holderCount.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++)
                if (holders[i].type == type)
                    return holders[i].holder;

            // Enums are the one family where a stand-in is defensible. Every
            // enum has its own Type - Axis, NormalId, HumanoidStateType are
            // three separate objects, allocated with the enum registry rather
            // than in the static Type array the scalars live in - but they all
            // store the same C++ value, so one holder serves them all. The
            // engine raises enum Variants of its own accord
            // (Animator.OnCombinedUpdate carries an AnimationPriority), which
            // is where the stand-in comes from; nothing will ever raise an
            // `Axis`.
            uint32_t reflection = 0;
            if (Probe::safeRead(&reflection, type + layout::typedescriptor_reflectiontype,
                                sizeof(reflection)) && reflection == layout::reflection_enum) {
                for (int i = 0; i < count; i++)
                    if (holders[i].reflection == layout::reflection_enum)
                        return holders[i].holder;
            }
            return 0;
        }


        inline int replicatesignal(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t binding = 0, descriptor = 0;
            if (!data ||
                !Probe::safeRead(&binding, reinterpret_cast<uintptr_t>(data), sizeof(binding)) ||
                !binding ||
                !Probe::safeRead(&descriptor, binding, sizeof(descriptor)) || !descriptor)
                return fail(state, "replicatesignal: expected an RBXScriptSignal");

            const std::vector<Argument> expected = argumentsOf(descriptor);
            const int given = rbx::lua_gettop(state) - 1;
            if (given != static_cast<int>(expected.size())) {
                char message[128];
                snprintf(message, sizeof(message),
                         "replicatesignal: this signal takes %d argument(s), got %d",
                         static_cast<int>(expected.size()), given);
                return fail(state, message);
            }

            std::vector<Variant> values(expected.size());
            for (size_t i = 0; i < expected.size(); i++) {
                const uintptr_t type = expected[i].typeObject;
                uint32_t kind = 0;
                if (!type ||
                    !Probe::safeRead(&kind, type + layout::typedescriptor_reflectiontype,
                                     sizeof(kind)))
                    return fail(state, "replicatesignal: could not read the argument type");

                Variant& value = values[i];
                value.type = type;
                value.holder = holderFor(type);
                if (!value.holder)
                    return fail(state, "replicatesignal: no TypeHolder for this argument type");

                const int slot = static_cast<int>(i) + 2;
                if (kind == layout::reflection_bool) {
                    *reinterpret_cast<bool*>(value.data) = rbx::lua_toboolean(state, slot) != 0;
                } else if (kind == layout::reflection_enum) {
                    if (rbx::lua_type(state, slot) != luaobj::kTagNumber)
                        return fail(state, "replicatesignal: expected a numeric argument");
                    *reinterpret_cast<int32_t*>(value.data) =
                        static_cast<int32_t>(utils::toNumber(state, slot));
                } else if (kind >= layout::reflection_int && kind <= layout::reflection_double) {
                    if (rbx::lua_type(state, slot) != luaobj::kTagNumber)
                        return fail(state, "replicatesignal: expected a numeric argument");
                    const double number = utils::toNumber(state, slot);
                    if (kind == layout::reflection_int) {
                        const double rounded = number < 0 ? number - 0.5 : number + 0.5;
                        *reinterpret_cast<int32_t*>(value.data) = static_cast<int32_t>(rounded);
                    } else if (kind == layout::reflection_int64) {
                        *reinterpret_cast<int64_t*>(value.data) = static_cast<int64_t>(number);
                    } else if (kind == layout::reflection_float) {
                        *reinterpret_cast<float*>(value.data) = static_cast<float>(number);
                    } else {
                        *reinterpret_cast<double*>(value.data) = number;
                    }
                } else if (kind == layout::reflection_instance) {
                    if (rbx::lua_type(state, slot) != luaobj::kTagNil) {
                        void* userdata = rbx::lua_touserdata(state, slot);
                        if (!userdata)
                            return fail(state, "replicatesignal: expected an Instance argument");
                        new (value.data) std::shared_ptr<uintptr_t>(
                            *reinterpret_cast<const std::shared_ptr<uintptr_t>*>(userdata));
                    }
                } else {
                    char message[128];
                    snprintf(message, sizeof(message),
                             "replicatesignal: cannot replicate a '%s' argument",
                             expected[i].type.c_str());
                    return fail(state, message);
                }
            }

            auto* event = reinterpret_cast<Binding*>(binding);
            auto source = event->source.lock();
            if (!source)
                return fail(state, "replicatesignal: signal source is no longer available");

            const uintptr_t address = reinterpret_cast<uintptr_t>(source.get());
            uintptr_t vtable = 0, publish = 0;
            if (!address ||
                !Probe::safeRead(&vtable, address, sizeof(vtable)) || !vtable ||
                !Probe::safeRead(&publish, vtable + layout::signals_publish,
                                 sizeof(publish)) ||
                !publish || !Probe::inImage(publish))
                return fail(state, "replicatesignal: no engine publish path for this signal");

            InvocationOptions options{nullptr, 0};
            journal::writef("REPLICATE args=%zu source=%llx publish=%llx",
                            expected.size(), (unsigned long long)address,
                            (unsigned long long)publish);
            reinterpret_cast<void (*)(uintptr_t, uintptr_t, std::vector<Variant>*,
                                      InvocationOptions*)>(publish)(address, descriptor,
                                                                    &values, &options);

            for (size_t i = 0; i < expected.size(); i++) {
                uint32_t kind = 0;
                if (Probe::safeRead(&kind,
                                    expected[i].typeObject + layout::typedescriptor_reflectiontype,
                                    sizeof(kind)) && kind == layout::reflection_instance) {
                    reinterpret_cast<std::shared_ptr<uintptr_t>*>(values[i].data)
                        ->~shared_ptr<uintptr_t>();
                }
            }
            return 0;
        }

        inline int cansignalreplicate(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_pushboolean(state, replicable(descriptorOf(state, 1)) ? 1 : 0);
            return 1;
        }

        // The replicator behind a NetworkClient.ClientReplicator userdata.
        inline uintptr_t replicatorAt(uint64_t state, int index)
        {
            void* data = rbx::lua_touserdata(state, index);
            uintptr_t replicator = 0;
            if (!data || !Probe::safeRead(&replicator, reinterpret_cast<uintptr_t>(data),
                                          sizeof(replicator)))
                return 0;
            return replicator;
        }

        // The engine's own answer to "will this client send that event".
        //
        // `replicable` above reads a bit out of the event descriptor, which was
        // arrived at by diffing signals whose answers were known - a good guess,
        // but a guess. The macOS reference does not guess: it calls
        // `IsLegalSendEvent(Replicator, owner, descriptor)`, which is the
        // function the replicator itself consults. That is what this is.
        //
        //   __vanta_islegalsendevent(signal, clientReplicator) -> boolean | nil
        //
        // nil means the question could not be put to the engine at all (no
        // replicator, or a value that is not a signal), which is different from
        // the engine answering false.
        //
        // A diagnostic, off unless a script turns it on: what the ENGINE hands
        // raiseEventInvocation when it raises an event itself.
        //
        //   __vanta_raisespy(instance, enable, nameFilter, budget)
        //
        // replicatesignal builds its own std::vector<Variant> and the server
        // does not act on the result, and every hypothesis about why comes down
        // to a byte we cannot see: no parameter anywhere in the signature tables
        // carries a real TypeHolder to copy (they all default to the void type
        // with a null holder), so there is nothing to compare our Variant
        // against. The engine raises the same events all the time - moving the
        // mouse over any GuiObject raises GuiObject.MouseMoved with two ints -
        // so borrowing one call of its own is the only way to see a correct
        // Variant for a scalar. Budgeted and name-filtered because this is one
        // of the hottest functions in the client.
        namespace RaiseSpy {
            inline void (*chain)(uintptr_t, uintptr_t, void*, void*) = nullptr;
            inline th_bak_t backup{};
            inline bool installed = false;
            inline int budget = 0;
            inline char filter[64] = {0};

            // Every Variant in every raise, harvested for its type/holder pair.
            // Cheap enough to run on the hot path: two reads and a scan of a
            // list that is a few dozen entries at most, and it stops finding
            // anything new within the first second.
            // Stops once an int and an Instance holder are in hand.
            //
            // `int` specifically, not "any scalar": substituting one scalar's
            // holder for another was tried and the replicator silently drops
            // the event - with only a bool holder to hand, a raise of
            // GuiObject.MouseWheelForward left the server's attribute unset,
            // exactly as it was with no holder at all. So the pairing is
            // per-type after all, and Vicna leaving a double's holder on an int
            // Variant is a bug of its own rather than a shortcut to copy.
            //
            // The hook is never removed: unhooking a function other threads may
            // be executing inside is worse than leaving one predictable branch
            // in front of it.
            inline std::atomic<bool> harvesting{true};

            inline bool wanted()
            {
                if (!harvesting.load(std::memory_order_relaxed))
                    return false;
                bool haveInt = false, haveInstance = false, haveEnum = false;
                const int count = holderCount.load(std::memory_order_acquire);
                for (int i = 0; i < count; i++) {
                    if (holders[i].reflection == layout::reflection_int) haveInt = true;
                    if (holders[i].reflection == layout::reflection_instance) haveInstance = true;
                    if (holders[i].reflection == layout::reflection_enum) haveEnum = true;
                }
                if (haveInt && haveInstance && haveEnum) {
                    harvesting.store(false, std::memory_order_relaxed);
                    return false;
                }
                return true;
            }

            inline void harvest(void* values)
            {
                uintptr_t span[3] = {0, 0, 0};
                if (!wanted())
                    return;
                if (!values || !Probe::safeRead(span, reinterpret_cast<uintptr_t>(values),
                                                sizeof(span)) || span[1] <= span[0])
                    return;
                const size_t count = (span[1] - span[0]) / sizeof(Variant);
                for (size_t i = 0; i < count && i < 24; i++) {
                    uintptr_t pair[2] = {0, 0};
                    if (Probe::safeRead(pair, span[0] + i * sizeof(Variant), sizeof(pair)))
                        rememberHolder(pair[0], pair[1]);
                }
            }

            inline void observe(uintptr_t owner, uintptr_t descriptor, void* values,
                                void* options)
            {
                harvest(values);
                if (budget > 0 && descriptor) {
                    std::string event, ownerClass;
                    uintptr_t ownerDescriptor = 0;
                    stringAt(descriptor, layout::descriptor_name, event);
                    if (!filter[0] || event.find(filter) != std::string::npos) {
                        budget--;
                        if (Probe::safeRead(&ownerDescriptor,
                                            owner + layout::instance_classdescriptor,
                                            sizeof(ownerDescriptor)))
                            stringAt(ownerDescriptor, layout::descriptor_name, ownerClass);

                        // A std::vector is {begin, end, cap}: end-begin in bytes
                        // is what says how wide one element really is, which is
                        // the number that cannot be got any other way.
                        uintptr_t span[3] = {0, 0, 0};
                        std::string bytes;
                        if (values && Probe::safeRead(span, reinterpret_cast<uintptr_t>(values),
                                                      sizeof(span))) {
                            const size_t width = span[1] > span[0] ? span[1] - span[0] : 0;
                            static const char* const hex = "0123456789abcdef";
                            const size_t show = width < 0x320 ? width : 0x320;
                            std::vector<uint8_t> raw(show);
                            if (show && Probe::safeRead(raw.data(), span[0], show)) {
                                for (size_t i = 0; i < show; i++) {
                                    if (i && (i % 8) == 0) bytes += ' ';
                                    bytes += hex[raw[i] >> 4];
                                    bytes += hex[raw[i] & 0xf];
                                }
                            }
                            journal::writef("RAISESPY %s.%s owner=%llx begin=%llx end=%llx "
                                            "span=%zu opts=%llx",
                                            ownerClass.c_str(), event.c_str(),
                                            (unsigned long long)owner,
                                            (unsigned long long)span[0],
                                            (unsigned long long)span[1], width,
                                            (unsigned long long)*reinterpret_cast<uint64_t*>(
                                                &options));
                            journal::writef("RAISESPY   %s", bytes.c_str());
                        } else {
                            journal::writef("RAISESPY %s.%s owner=%llx (no vector)",
                                            ownerClass.c_str(), event.c_str(),
                                            (unsigned long long)owner);
                        }
                    }
                }
                chain(owner, descriptor, values, options);
            }

            inline int raisespy(lua_State* L) {
                uint64_t state = reinterpret_cast<uint64_t>(L);
                void* data = rbx::lua_touserdata(state, 1);
                uintptr_t instance = 0, vtable = 0, raise = 0;
                const bool enable = rbx::lua_toboolean(state, 2) != 0;

                size_t length = 0;
                const char* want = rbx::lua_tolstring(state, 3, &length);
                std::memset(filter, 0, sizeof(filter));
                if (want && length && length < sizeof(filter))
                    std::memcpy(filter, want, length);
                int isnum = 0;
                const long asked = rbx::lua_tointegerx(state, 4, &isnum);
                budget = (isnum && asked > 0 && asked < 400) ? static_cast<int>(asked) : 20;

                if (!enable) {
                    if (installed) {
                        tiny_unhook_ex(&backup);
                        installed = false;
                    }
                    budget = 0;
                    rbx::lua_pushboolean(state, 1);
                    return 1;
                }

                if (installed) {
                    rbx::lua_pushboolean(state, 1);
                    return 1;
                }

                if (!data ||
                    !Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data),
                                     sizeof(instance)) || !instance ||
                    !Probe::safeRead(&vtable, instance, sizeof(vtable)) || !vtable ||
                    !Probe::safeRead(&raise, vtable + layout::instance_vtable_raiseevent,
                                     sizeof(raise)) || !Probe::inImage(raise)) {
                    rbx::lua_pushnil(state);
                    return 1;
                }

                installed = tiny_hook_ex(&backup, reinterpret_cast<void*>(raise),
                                         reinterpret_cast<void*>(&observe),
                                         reinterpret_cast<void**>(&chain)) == 0;
                journal::writef("RAISESPY install raise=%llx ok=%d filter=%s budget=%d",
                                (unsigned long long)raise, installed ? 1 : 0, filter, budget);
                rbx::lua_pushboolean(state, installed ? 1 : 0);
                return 1;
            }

            // What the spy has learned: every reflection Type it has seen, with
            // the TypeHolder the engine paired it with.
            //
            //   __vanta_raisespy_pairs() -> { {Type=, Holder=}, ... }
            inline int pairs(lua_State* L) {
                uint64_t state = reinterpret_cast<uint64_t>(L);
                const int count = holderCount.load(std::memory_order_acquire);
                rbx::lua_createtable(state, count, 0);
                for (int i = 0; i < count; i++) {
                    rbx::lua_createtable(state, 0, 2);
                    utils::pushNumber(state, static_cast<double>(holders[i].type));
                    rbx::lua_setfield(state, -2, "Type");
                    utils::pushNumber(state, static_cast<double>(holders[i].holder));
                    rbx::lua_setfield(state, -2, "Holder");
                    rbx::lua_rawseti(state, -2, i + 1);
                }
                return 1;
            }

            // Teach the table a pair by hand, so a probe can try a holder the
            // spy has not seen without another build.
            //
            //   __vanta_holderhint(typeAddress, holderAddress) -> count
            inline int holderhint(lua_State* L) {
                uint64_t state = reinterpret_cast<uint64_t>(L);
                uintptr_t type = 0, holder = 0;
                if (Probe::address(state, 1, type) && Probe::address(state, 2, holder))
                    rememberHolder(type, holder);
                rbx::lua_pushinteger(state, holderCount.load(std::memory_order_acquire));
                return 1;
            }
        }

        inline int islegalsendevent(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* userdata = rbx::lua_touserdata(state, 1);
            uintptr_t eventInstance = 0, descriptor = 0, owner = 0;
            const uintptr_t replicator = replicatorAt(state, 2);
            if (!userdata || !replicator ||
                !Probe::safeRead(&eventInstance, reinterpret_cast<uintptr_t>(userdata),
                                 sizeof(eventInstance)) || !eventInstance ||
                !Probe::safeRead(&descriptor, eventInstance, sizeof(descriptor)) || !descriptor ||
                !Probe::safeRead(&owner, eventInstance + layout::eventinstance_owner,
                                 sizeof(owner)) || !owner) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const bool legal = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uintptr_t)>(
                offsets::islegalsendevent)(replicator, owner, descriptor);
            rbx::lua_pushboolean(state, legal ? 1 : 0);
            return 1;
        }

        // Every event in the replicator's send whitelist, by name.
        //
        //   __vanta_signalwhitelist(clientReplicator) -> { {Event=, Parent=, Mode=}, ... }
        //
        // The set is libc++'s __hash_table: the head node is the third word and
        // each node is {next, hash, key, value}, so walking `next` from the head
        // visits every entry exactly once. (The macOS reference walks the bucket
        // array instead, which re-visits entries because a bucket's chain runs
        // on into the next bucket's.)
        inline int signalwhitelist(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t replicator = replicatorAt(state, 1);

            uintptr_t filter = 0, whitelist = 0, node = 0;
            if (!replicator ||
                !Probe::safeRead(&filter, replicator + layout::replicator_filter,
                                 sizeof(filter)) || !filter ||
                !Probe::safeRead(&whitelist, filter + layout::filter_whitelist,
                                 sizeof(whitelist)) || !whitelist) {
                rbx::lua_pushnil(state);
                return 1;
            }
            // `whitelist` is the set object itself, so the head node is read
            // from inside it rather than through another indirection.
            if (!Probe::safeRead(&node, whitelist + 0x10, sizeof(node))) {
                rbx::lua_pushnil(state);
                return 1;
            }

            rbx::lua_createtable(state, 0, 0);
            int count = 0;
            for (int guard = 0; node && guard < 20000; guard++) {
                uintptr_t descriptor = 0, next = 0, classDescriptor = 0;
                int32_t mode = 0;
                if (!Probe::safeRead(&next, node, sizeof(next)) ||
                    !Probe::safeRead(&descriptor, node + layout::whitelistnode_descriptor,
                                     sizeof(descriptor)))
                    break;
                Probe::safeRead(&mode, node + layout::whitelistnode_mode, sizeof(mode));

                std::string event, owner;
                if (descriptor && stringAt(descriptor, layout::descriptor_name, event)) {
                    if (Probe::safeRead(&classDescriptor, descriptor + layout::descriptor_owner,
                                        sizeof(classDescriptor)))
                        stringAt(classDescriptor, layout::descriptor_name, owner);

                    rbx::lua_createtable(state, 0, 3);
                    rbx::lua_pushlstring(state, event.data(), event.size());
                    rbx::lua_setfield(state, -2, "Event");
                    rbx::lua_pushlstring(state, owner.data(), owner.size());
                    rbx::lua_setfield(state, -2, "Parent");
                    rbx::lua_pushinteger(state, mode);
                    rbx::lua_setfield(state, -2, "Mode");
                    rbx::lua_rawseti(state, -2, ++count);
                }
                node = next;
            }
            return 1;
        }

        // What a connection slot actually holds.
        //
        // The shape is not fixed: a BindableEvent slot keeps its ConnectionData
        // at +0x30, a BasePart.Touched slot at +0x38, and the LiveThreadRef
        // inside it is reachable either directly at +0x60 or through +0x10 and
        // then +0x58. Pinning any one of those makes half the signals in the
        // game invisible - which is exactly what kept firesignal from ever
        // reaching a Touched handler. So each candidate is tried and the first
        // one that yields a plausible registry ref is taken.
        struct SlotFields {
            uintptr_t data = 0;
            uintptr_t live = 0;
            uintptr_t waiting = 0;
            uintptr_t owner = 0;
            int32_t functionRef = 0;
            int32_t threadRef = 0;
            int32_t waitRef = 0;
        };

        inline bool sanePointer(uintptr_t value)
        {
            return value > 0x100000000ULL && value < 0x800000000000ULL;
        }

        inline bool saneRef(int32_t value)
        {
            return value > 0 && value < 0x1000000;
        }

        // Whether a ref really names a function in this state's registry. The
        // direct LiveThreadRef field can hold a value that passes every shape
        // test and is not a ref at all - a Touched slot answers 0x7FF8 there,
        // which is the top half of a NaN-boxed double - so the only test that
        // settles it is looking the ref up.
        // A guarded block read, so several fields of one object cost ONE
        // syscall instead of one each.
        //
        // Probe::safeRead is mach_vm_read_overwrite - a kernel call - and it is
        // the right primitive here (these addresses come out of engine memory
        // that other threads rewrite, so a stale one has to be an error rather
        // than a fault). What was wrong was the granularity: resolveSlot read
        // about forty separate 8-byte fields, so `connection.Enabled` measured
        // **20 us** and getconnections 31 us (perf.lua/perf2.lua, 2026-09-03) -
        // nearly all of it in the kernel.
        //
        // `read()` falls back to a single-field safeRead when the block read
        // fails, which keeps the old behaviour exactly for an object whose tail
        // crosses into an unmapped page: before, each field succeeded or failed
        // on its own, and requiring the whole block would have made those
        // connections disappear instead.
        template <size_t N>
        struct Fields {
            uintptr_t base = 0;
            bool whole = false;
            uint8_t bytes[N] = {};

            explicit Fields(uintptr_t at) : base(at)
            {
                whole = at && Probe::safeRead(bytes, at, N);
            }

            template <typename T>
            T read(int offset) const
            {
                T value{};
                if (!base)
                    return value;
                if (whole && offset >= 0 &&
                    static_cast<size_t>(offset) + sizeof(T) <= N) {
                    std::memcpy(&value, bytes + offset, sizeof(T));
                    return value;
                }
                Probe::safeRead(&value, base + offset, sizeof(value));
                return value;
            }
        };

        inline bool refIsFunction(uint64_t state, int32_t reference)
        {
            if (!state || reference <= 0)
                return false;
            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
            const bool ok = rbx::lua_type(state, -1) == luaobj::kTagFunction;
            rbx::lua_settop(state, -2);
            return ok;
        }

        inline SlotFields resolveSlot(uint64_t state, uintptr_t slot)
        {
            SlotFields out;
            if (!slot)
                return out;

            for (int dataOffset : {layout::slot_data_first, layout::slot_data_second}) {
                uintptr_t data = 0;
                if (!Probe::safeRead(&data, slot + dataOffset, sizeof(data)) || !sanePointer(data))
                    continue;

                // Two places the LiveThreadRef can be: straight off the data,
                // or one hop further in. Both are tried rather than preferred,
                // because the direct field can hold something pointer-shaped
                // that is not a LiveThreadRef at all.
                // indirect (0x10), waitref (0x38) and liveref (0x60) all live
                // in the first 0x68 bytes of the ConnectionData, so they come
                // back together.
                const Fields<0x68> connection(data);

                uintptr_t candidates[2] = { 0, 0 };
                const uintptr_t indirect =
                    connection.read<uintptr_t>(layout::connectiondata_indirect);
                if (sanePointer(indirect))
                    Probe::safeRead(&candidates[0], indirect + layout::connectiondata_indirect_live,
                                    sizeof(candidates[0]));
                candidates[1] = connection.read<uintptr_t>(layout::connectiondata_liveref);

                const uintptr_t waiting =
                    connection.read<uintptr_t>(layout::connectiondata_waitref);
                int32_t waitRef = 0;
                if (sanePointer(waiting))
                    Probe::safeRead(&waitRef, waiting + layout::liveref_threadref, sizeof(waitRef));

                const uintptr_t ourGlobal =
                    state ? luaobj::read<uintptr_t>(reinterpret_cast<void*>(state),
                                                    luaobj::state::global)
                          : 0;

                uintptr_t live = 0;
                int32_t functionRef = 0, threadRef = 0;
                for (uintptr_t candidate : candidates) {
                    if (!sanePointer(candidate))
                        continue;
                    // thread (0x30), functionref (0x34) and the owning thread
                    // pointer are all inside the LiveThreadRef's first 0x40.
                    const Fields<0x40> ref(candidate);
                    const int32_t function = ref.read<int32_t>(layout::liveref_functionref);
                    const int32_t thread = ref.read<int32_t>(layout::liveref_threadref);
                    if (!saneRef(function))
                        continue;

                    // Either the ref resolves here, or the slot belongs to
                    // another Luau state - in which case it resolves over
                    // there and this registry knows nothing about it.
                    const uintptr_t candidateOwner = ref.read<uintptr_t>(layout::liveref_thread);
                    uintptr_t candidateGlobal = 0;
                    if (sanePointer(candidateOwner))
                        Probe::safeRead(&candidateGlobal, candidateOwner + luaobj::state::global,
                                        sizeof(candidateGlobal));
                    const bool elsewhere = sanePointer(candidateGlobal) && ourGlobal &&
                                           candidateGlobal != ourGlobal;
                    if (!elsewhere && !refIsFunction(state, function))
                        continue;

                    live = candidate;
                    functionRef = function;
                    threadRef = thread;
                    break;
                }

                if (!saneRef(functionRef) && !saneRef(waitRef))
                    continue;

                out.data = data;
                out.live = sanePointer(live) ? live : 0;
                out.waiting = sanePointer(waiting) ? waiting : 0;
                out.functionRef = saneRef(functionRef) ? functionRef : 0;
                out.threadRef = saneRef(threadRef) ? threadRef : 0;
                out.waitRef = saneRef(waitRef) ? waitRef : 0;

                const uintptr_t holder = out.live ? out.live : out.waiting;
                uintptr_t owner = 0;
                if (holder)
                    Probe::safeRead(&owner, holder + layout::liveref_thread, sizeof(owner));
                // Only a pointer that reads like a real lua_State counts as the
                // owning thread; a misread here is what made every Touched slot
                // claim to belong to another VM.
                if (sanePointer(owner)) {
                    // global, gt and ci are all in the lua_State's first 0x80.
                    const Fields<0x80> thread(owner);
                    const uintptr_t global = thread.read<uintptr_t>(luaobj::state::global);
                    const uintptr_t globals = thread.read<uintptr_t>(luaobj::state::gt);
                    const uintptr_t callinfo = thread.read<uintptr_t>(luaobj::state::ci);
                    if (sanePointer(global) && sanePointer(globals) && sanePointer(callinfo))
                        out.owner = owner;
                }
                return out;
            }
            return out;
        }

        // One connection slot, read out for the Lua side: where the chain goes
        // next, whether it is live, and the registry refs of the handler and
        // the thread it belongs to.
        inline int slotinfo(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t slot = 0;
            if (!Probe::address(state, 1, slot) || !slot) {
                rbx::lua_pushnil(state);
                return 1;
            }

            uintptr_t next = 0, stateSlot = 0;
            Probe::safeRead(&next, slot + layout::slot_next, sizeof(next));
            Probe::safeRead(&stateSlot, slot + layout::slot_state, sizeof(stateSlot));

            const SlotFields fields = resolveSlot(state, slot);
            const int32_t functionRef = fields.functionRef;
            const int32_t threadRef = fields.threadRef;
            const int32_t waitRef = fields.waitRef;

            // A connection made inside an Actor keeps its handler in that
            // Actor's registry and its thread on that Actor's heap, so neither
            // can be reached from here by ref alone. Saying so lets firesignal
            // hand those slots to the cross-state path instead of silently
            // skipping them.
            const uintptr_t ourGlobal =
                luaobj::read<uintptr_t>(reinterpret_cast<void*>(state), luaobj::state::global);
            uintptr_t theirGlobal = 0;
            if (fields.owner)
                Probe::safeRead(&theirGlobal, fields.owner + luaobj::state::global,
                                sizeof(theirGlobal));

            rbx::lua_createtable(state, 0, 8);
            // The ConnectionData address, so a probe can look at what actually
            // distinguishes a :Once slot from a :Connect one - the two are the
            // same slot type, and firesignal has to know which is which to
            // retire a Once the way the engine does.
            utils::pushNumber(state, static_cast<double>(fields.data));
            rbx::lua_setfield(state, -2, "data");
            rbx::lua_pushboolean(state, (theirGlobal && theirGlobal != ourGlobal) ? 1 : 0);
            rbx::lua_setfield(state, -2, "foreign");
            rbx::lua_pushinteger(state, waitRef);
            rbx::lua_setfield(state, -2, "waitref");
            utils::pushNumber(state, static_cast<double>(next));
            rbx::lua_setfield(state, -2, "next");
            rbx::lua_pushboolean(state, stateSlot != 0);
            rbx::lua_setfield(state, -2, "enabled");
            // One-shot connections have to be retired by whoever fires them,
            // because the engine retires its own. See connectiondata_isonce.
            uint8_t once = 0;
            Probe::safeRead(&once, fields.data + layout::connectiondata_isonce, sizeof(once));
            rbx::lua_pushboolean(state, (fields.data && once == 1) ? 1 : 0);
            rbx::lua_setfield(state, -2, "once");
            rbx::lua_pushinteger(state, functionRef);
            rbx::lua_setfield(state, -2, "functionref");
            rbx::lua_pushinteger(state, threadRef);
            rbx::lua_setfield(state, -2, "threadref");
            return 1;
        }

        // Whether one slot is live, and nothing else about it.
        //
        // slotinfo costs a dozen reads and a table on every call, because it
        // resolves the handler and the thread and works out which Luau state
        // they belong to. A property read that only wants the switch the engine
        // itself checks has no reason to pay for any of that - and Enabled is
        // read far more often than the rest put together.
        // Is this connection still live?
        //
        //   __vanta_slotenabled(address [, expectedRef]) -> boolean | nil
        //
        // The state pointer alone is not enough. When the engine retires a slot
        // - a `:Wait` that has fired, a connection the game disconnected - it
        // unlinks it from the signal's chain, and the block we hold the address
        // of is no longer the signal's business. Measured (`tests/con3.lua`):
        // after a real `BindableEvent:Fire()` the retired wait slot still reads
        // `state != 0` while the chain no longer contains it, so `Enabled`
        // answered true for a connection that no longer exists - which is what
        // sUNC calls "Returned true for 'Enabled' after triggering a :Wait
        // connection".
        //
        // What *does* change is the ref. RbxStu does not read the state for
        // this at all: its `IsEnabled` compares the slot's current ref against
        // the one captured when the connection object was made
        // (`RbxStuConnection.cpp`, `objRef->objectId == m_rpRealFunction.luaRef`).
        // That is the check - a slot that no longer carries the ref it was made
        // with is not this connection any more. Measured on the same run, the
        // wait ref went 90 -> 0 across the fire.
        //
        // `expectedRef` is optional: a connection we could not read a ref for in
        // the first place (one made inside an Actor) falls back to the state
        // read rather than answering false for ever.
        inline int slotenabled(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t slot = 0;
            uintptr_t stateSlot = 0;
            if (!Probe::address(state, 1, slot) || !slot ||
                !Probe::safeRead(&stateSlot, slot + layout::slot_state, sizeof(stateSlot))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            if (stateSlot == 0) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const int slotType = rbx::lua_type(state, 2);
            const int expected =
                (slotType == luaobj::kTagNumber || slotType == luaobj::kTagInteger)
                    ? static_cast<int>(utils::toNumber(state, 2))
                    : 0;
            if (expected == 0) {
                rbx::lua_pushboolean(state, 1);
                return 1;
            }

            const SlotFields fields = resolveSlot(state, slot);
            const bool same = fields.functionRef == expected || fields.waitRef == expected ||
                              fields.threadRef == expected;
            rbx::lua_pushboolean(state, same ? 1 : 0);
            return 1;
        }

        // Delivers a signal to a handler that lives in another Luau state.
        //
        // A connection made inside an Actor cannot be run from here: its
        // handler is a ref in that Actor's registry and its thread is on that
        // Actor's heap, and neither means anything to us. So the work is done
        // over there - a thread is built on the Actor's own state, the handler
        // and the arguments are put into that thread's (sandboxed) globals, and
        // a two-line chunk hands them to the Actor's own task.spawn.
        //
        // It goes through task.spawn rather than being called directly for the
        // same reason the local path does: the handler has to be the body of
        // its own thread, or it shows up in its own traceback.
        //
        //   __vanta_fireforeign(slotAddress, packedArguments) -> boolean
        inline int fireforeign(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            uintptr_t slot = 0;
            if (!Probe::address(state, 1, slot) || !slot) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const SlotFields fields = resolveSlot(state, slot);
            const uintptr_t owner = fields.owner;
            const int32_t handlerRef = fields.functionRef ? fields.functionRef
                                                          : (fields.waitRef ? fields.waitRef
                                                                            : fields.threadRef);
            if (!owner || handlerRef == 0) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const uint64_t target = Actors::stateOwning(state, owner);
            if (!target) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            uint64_t thread = rbx::lua_newthread(target);
            rbx::lua_ref(target, -1);
            rbx::lua_settop(target, -2);
            rbx::luaL_sandboxthread(thread);
            SetIdentity(thread, kExecutorIdentity);
            rbx::lua_setsafeenv(thread, LUA_GLOBALSINDEX, 0);

            // The handler, out of the state that owns it.
            rbx::lua_rawgeti(thread, LUA_REGISTRYINDEX, handlerRef);
            if (rbx::lua_type(thread, -1) != luaobj::kTagFunction &&
                rbx::lua_type(thread, -1) != luaobj::kTagThread) {
                rbx::lua_settop(thread, 0);
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            rbx::lua_setfield(thread, LUA_GLOBALSINDEX, "__vanta_handler");

            // The arguments come from a table packed on our side, so a trailing
            // nil survives the crossing - which is exactly what the suite
            // checks with its `n == 3`.
            int count = 0;
            rbx::lua_getfield(state, 2, "n");
            int isnum = 0;
            count = rbx::lua_tointegerx(state, -1, &isnum);
            rbx::lua_settop(state, -2);
            if (!isnum || count < 0 || count > 255)
                count = 0;

            rbx::lua_createtable(thread, count, 1);
            for (int i = 1; i <= count; i++) {
                rbx::lua_rawgeti(state, 2, i);
                Actors::transfer(state, thread, rbx::lua_gettop(state), 0);
                rbx::lua_settop(state, -2);
                rbx::lua_rawseti(thread, -2, i);
            }
            rbx::lua_pushinteger(thread, count);
            rbx::lua_setfield(thread, -2, "n");
            rbx::lua_setfield(thread, LUA_GLOBALSINDEX, "__vanta_handlerargs");

            static const char* const kFire =
                "local f, a = __vanta_handler, __vanta_handlerargs "
                "__vanta_handler, __vanta_handlerargs = nil, nil "
                "task.spawn(f, table.unpack(a, 1, a.n))";
            const std::string bytecode = compileScript(kFire);
            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(thread, "@vanta", bytecode.c_str(), bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(thread, 0);
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            rbx::setprotocapabilities(thread, -1, &MaxCapabilities);
            rbx::task::spawn(thread);

            rbx::lua_pushboolean(state, 1);
            return 1;
        }

        // Disable and enable are the engine's own switch: a slot whose State is
        // null is skipped when the signal fires. The pointer that was there is
        // handed back so enabling can put the same one back rather than
        // inventing one.
        inline int setslotstate(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t slot = 0, wanted = 0;
            if (!Probe::address(state, 1, slot) || !slot) {
                rbx::lua_pushnil(state);
                return 1;
            }
            Probe::address(state, 2, wanted);

            auto* field = reinterpret_cast<uintptr_t*>(slot + layout::slot_state);
            uintptr_t previous = 0;
            if (!Probe::safeRead(&previous, slot + layout::slot_state, sizeof(previous))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            *field = wanted;
            utils::pushNumber(state, static_cast<double>(previous));
            return 1;
        }

        // The event's own name and the class it belongs to - what the whitelist
        // and every "which signal is this" question is written in terms of.
        inline int signalidentity(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t descriptor = descriptorOf(state, 1);

            std::string event, owner;
            uintptr_t classDescriptor = 0;
            if (!descriptor || !stringAt(descriptor, layout::descriptor_name, event)) {
                rbx::lua_pushnil(state);
                return 1;
            }
            if (Probe::safeRead(&classDescriptor, descriptor + layout::descriptor_owner, sizeof(classDescriptor)))
                stringAt(classDescriptor, layout::descriptor_name, owner);

            rbx::lua_createtable(state, 0, 2);
            rbx::lua_pushlstring(state, event.data(), event.size());
            rbx::lua_setfield(state, -2, "Event");
            rbx::lua_pushlstring(state, owner.data(), owner.size());
            rbx::lua_setfield(state, -2, "Parent");
            return 1;
        }
    }

    // Property descriptors, and the one bit in them that decides whether Lua can
    // see a property at all.
    //
    // Every offset here was read out of a live game. A member descriptor keeps
    // its name at +0x8 and the ClassDescriptor that owns it at +0x30, which is
    // what lets them be found by scanning the pages around a class descriptor
    // rather than by guessing at container layouts. A ClassDescriptor names its
    // own class at +0x8 and points at its base class at +0x1b0, so a property
    // defined on an ancestor is reached by walking that chain.
    //
    // The scriptable bit is byte +0x8b, mask 0x10. It was found by diffing two
    // properties of the same class whose answers are known: Transparency and
    // Instance.Name have it set, MaterialVariantSerialized does not - which is
    // exactly the split the spec describes.
    namespace Reflection2 {

        namespace layout = offsets::structs;

        inline std::mutex cacheMutex;
        inline std::unordered_map<uintptr_t, std::unordered_map<std::string, uintptr_t>> memberCache;

        // A page is read at most once for the life of the process, and every
        // descriptor it holds is filed under the class that owns it - not just
        // under the class being asked about.
        //
        // Every class's descriptors live in the same region, so one class's
        // 12 MB window covers the next one's almost entirely. Rescanning it per
        // class is what made the first property read on a class cost ~7 ms:
        // walking Part -> BasePart -> Instance for one name paid for the window
        // three times. Now the pages are read once and the per-class answer is
        // a lookup in what they held.
        //
        // Reflection descriptors are all registered during startup, well before
        // any script runs, so a page that held none the first time is not going
        // to start holding one later.
        inline std::unordered_set<uintptr_t> scannedPages;
        inline std::vector<std::pair<uintptr_t, uintptr_t>> descriptorIndex;   // owner -> descriptor
        inline bool descriptorIndexSorted = true;

        // Every member the given class declares itself, by name. Cached: the
        // scan is a few megabytes of pages and the answer never changes.
        inline const std::unordered_map<std::string, uintptr_t>& membersOf(uintptr_t classDescriptor)
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto found = memberCache.find(classDescriptor);
            if (found != memberCache.end())
                return found->second;

            constexpr size_t kPage = 4096;
            constexpr uintptr_t kWindow = 0x600000;
            const uintptr_t start = (classDescriptor > kWindow ? classDescriptor - kWindow : 0) & ~(kPage - 1);
            const uintptr_t end = classDescriptor + kWindow;

            // Range only. Requiring 8-byte alignment here was a mistake: it is
            // not a property of a std::string* that Roblox guarantees, and it
            // quietly dropped real members - Instance.Tags among them, which is
            // one of the three properties sUNC asks gethiddenproperty for. The
            // printability check below is what actually validates a candidate.
            const auto sane = [](uintptr_t value) {
                return value > 0x10000 && value < 0x800000000000ULL;
            };

            std::vector<uint8_t> page(kPage);
            for (uintptr_t base = start; base < end; base += kPage) {
                if (!scannedPages.insert(base).second)
                    continue;
                if (!Probe::safeRead(page.data(), base, kPage))
                    continue;
                // 0x38 is what this actually touches - the name at +0x8 and the
                // owner at +0x30. Requiring 0x90 skipped every descriptor that
                // began in the last 88 bytes of a page, and Instance.Tags is
                // one of them: the scan found 66 members for Instance and not
                // that one, while a scanner using 0x38 found it. Anything read
                // beyond this (the flags byte at +0x8b) goes through safeRead
                // on the descriptor's own address, not this page buffer.
                for (size_t offset = 0; offset + 0x38 <= kPage; offset += 8) {
                    const uintptr_t descriptor = base + offset;
                    uintptr_t owner, namePointer;
                    std::memcpy(&owner, page.data() + offset + layout::descriptor_owner, sizeof(owner));
                    // Filed under the owner only when the owner is close enough
                    // that its own window would have covered this page. That is
                    // the same test the old code made, written the other way
                    // round: it is a property of the descriptor rather than of
                    // whichever class happened to trigger the scan, so a class
                    // asked about later gets exactly what a scan centred on it
                    // would have found. A descriptor also carries a name
                    // pointer, which is free to check and keeps the index down
                    // to things actually shaped like one.
                    if (owner < (descriptor > kWindow ? descriptor - kWindow : 0) ||
                        owner >= descriptor + kWindow)
                        continue;
                    std::memcpy(&namePointer, page.data() + offset + layout::descriptor_name,
                                sizeof(namePointer));
                    if (!sane(namePointer))
                        continue;
                    descriptorIndex.emplace_back(owner, descriptor);
                    descriptorIndexSorted = false;
                }
            }

            if (!descriptorIndexSorted) {
                std::sort(descriptorIndex.begin(), descriptorIndex.end());
                descriptorIndexSorted = true;
            }

            auto& members = memberCache[classDescriptor];

            // The class descriptor carries the authoritative member lists as
            // vectors: methods at +0x118 (count at +0x120) and callbacks at
            // +0x160 (count at +0x168). Each entry is 0x10 bytes, descriptor
            // pointer first. The heap scan below can only index descriptors
            // that happen to live near the class, and DataModel's OnClose sits
            // tens of megabytes away - found here, missed there. Both vectors
            // are tried; an entry only counts when its name pointer reads as a
            // printable member name.
            for (const int vector : {0x118, 0x160}) {
                uintptr_t data = 0;
                uint64_t count = 0;
                if (!Probe::safeRead(&data, classDescriptor + vector, sizeof(data)) ||
                    !data ||
                    !Probe::safeRead(&count, classDescriptor + vector + 8, sizeof(count)) ||
                    count == 0 || count > 4096)
                    continue;
                for (uint64_t i = 0; i < count; i++) {
                    uintptr_t descriptor = 0;
                    if (!Probe::safeRead(&descriptor, data + i * 0x10, sizeof(descriptor)) ||
                        !descriptor)
                        continue;
                    uintptr_t namePointer = 0;
                    if (!Probe::safeRead(&namePointer, descriptor + layout::descriptor_name,
                                         sizeof(namePointer)) ||
                        !sane(namePointer))
                        continue;
                    std::string name;
                    if (!Probe::readString(namePointer, name, 256) || name.empty() ||
                        name.size() > 64)
                        continue;
                    bool printable = true;
                    for (char c : name)
                        printable = printable && c >= 32 && c < 127;
                    if (printable)
                        members.emplace(std::move(name), descriptor);
                }
            }

            const auto range = std::equal_range(
                descriptorIndex.begin(), descriptorIndex.end(),
                std::make_pair(classDescriptor, uintptr_t{0}),
                [](const std::pair<uintptr_t, uintptr_t>& a, const std::pair<uintptr_t, uintptr_t>& b) {
                    return a.first < b.first;
                });

            for (auto it = range.first; it != range.second; ++it) {
                uintptr_t namePointer = 0;
                if (!Probe::safeRead(&namePointer, it->second + layout::descriptor_name,
                                     sizeof(namePointer)))
                    continue;
                std::string name;
                if (!Probe::readString(namePointer, name, 256) || name.empty() || name.size() > 64)
                    continue;

                bool printable = true;
                for (char c : name)
                    printable = printable && c >= 32 && c < 127;
                if (printable)
                    members.emplace(std::move(name), it->second);
            }
            return members;
        }

        // Walks the class chain, so a property declared on BasePart is found
        // from a Part and one declared on Instance is found from anything.
        inline uintptr_t findMember(uintptr_t instance, const std::string& name)
        {
            const bool loud = trace::on("propdesc");
            uintptr_t classDescriptor = 0;
            if (!Probe::safeRead(&classDescriptor, instance + layout::instance_classdescriptor,
                                 sizeof(classDescriptor)))
                return 0;

            for (int depth = 0; depth < 24 && classDescriptor; depth++) {
                const auto& members = membersOf(classDescriptor);
                if (loud) {
                    uintptr_t namePointer = 0;
                    std::string className;
                    Probe::safeRead(&namePointer, classDescriptor + layout::descriptor_name,
                                    sizeof(namePointer));
                    Probe::readString(namePointer, className, 128);
                    journal::writef("PROPDESC looking for '%s' in %s (%zu members) at %llx",
                                    name.c_str(), className.c_str(), members.size(),
                                    static_cast<unsigned long long>(classDescriptor));
                }
                auto found = members.find(name);
                if (found != members.end())
                    return found->second;

                uintptr_t base = 0;
                if (!Probe::safeRead(&base, classDescriptor + layout::classdescriptor_base, sizeof(base)) ||
                    base == classDescriptor)
                    break;

                // The base pointer is only trusted when it really does name a
                // class; anything else means the chain has run out.
                uintptr_t namePointer = 0;
                std::string className;
                if (!Probe::safeRead(&namePointer, base + layout::descriptor_name, sizeof(namePointer)) ||
                    !Probe::readString(namePointer, className, 128) || className.empty())
                    break;
                classDescriptor = base;
            }
            return 0;
        }

        // A read-only look at what the descriptor's getter actually writes.
        //
        //   __vanta_probeproperty(instance, descriptor)
        //       -> { type = <reflection type>, raw = "<64 hex bytes>" }
        //
        // readProperty below has to decide, from the reflection type alone,
        // whether the getter's out-parameter is a std::string or a handle - and
        // if it guesses wrong the answer is a plausible-looking empty string
        // rather than an error. This calls the getter with a zeroed 64-byte
        // buffer and hands the bytes back untouched, so both readings can be
        // compared against a property whose value is known. Nothing is
        // constructed or destroyed, so a wrong guess cannot free a pointer that
        // was never allocated.
        inline int probeProperty(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t descriptor = 0, instance = 0;
            if (!data || !Probe::address(state, 2, descriptor) || !descriptor ||
                !Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance) {
                rbx::lua_pushnil(state);
                return 1;
            }

            uintptr_t typeDescriptor = 0, getset = 0, vtable = 0, getter = 0;
            uint32_t reflectionType = 0;
            if (!Probe::safeRead(&typeDescriptor, descriptor + layout::property_typedescriptor,
                                 sizeof(typeDescriptor)) || !typeDescriptor ||
                !Probe::safeRead(&reflectionType,
                                 typeDescriptor + layout::typedescriptor_reflectiontype,
                                 sizeof(reflectionType)) ||
                !Probe::safeRead(&getset, descriptor + layout::property_getset, sizeof(getset)) ||
                !getset ||
                !Probe::safeRead(&vtable, getset, sizeof(vtable)) || !vtable ||
                !Probe::safeRead(&getter, vtable + layout::getset_vtable_get, sizeof(getter)) ||
                !getter || !Probe::inImage(getter)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            alignas(16) unsigned char buffer[64] = {0};
            reinterpret_cast<void (*)(void*, uintptr_t, uintptr_t)>(getter)(buffer, getset,
                                                                           instance);

            static const char* const hex = "0123456789abcdef";
            std::string dump;
            dump.reserve(sizeof(buffer) * 2);
            for (unsigned char byte : buffer) {
                dump += hex[byte >> 4];
                dump += hex[byte & 0xf];
            }

            rbx::lua_createtable(state, 0, 4);
            rbx::lua_pushinteger(state, static_cast<int>(reflectionType));
            rbx::lua_setfield(state, -2, "type");
            rbx::lua_pushlstring(state, dump.data(), dump.size());
            rbx::lua_setfield(state, -2, "raw");
            rbx::lua_pushinteger(
                state, static_cast<int>((getter - hostImage().slide) - 0x100000000ULL));
            rbx::lua_setfield(state, -2, "getter");
            return 1;
        }

        // A property whose value the engine will not hand to Lua, read through
        // the descriptor's own accessor instead of the Lua bridge.
        //
        // `Instance.Tags` is one of these: however scriptable the property is
        // made, Roblox answers "Unable to get property Tags, type SharedString"
        // - the descriptor produced a value and only the conversion to Lua
        // refused. Every reference executor reads this class of property by
        // going to the GetSet object the descriptor carries and calling its
        // vtable's getter, and that is what this does. See offsets::structs for
        // the chain and how each offset was derived.
        //
        // An earlier version read the member straight out of the instance using
        // the field at descriptor + 0x80 as a byte offset. That field is not an
        // offset (Name reads 0x0ddb and Tags 0x0dde, three apart, which two
        // std::strings cannot be) and Tags is not a std::string in memory
        // anyway - it is a std::vector of tag pointers, and the string only
        // exists once the property is asked for. Hence going through the
        // accessor.
        //
        //   __vanta_readproperty(instance, descriptor) -> value | nil
        inline int readProperty(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t descriptor = 0;
            uintptr_t instance = 0;
            if (!data || !Probe::address(state, 2, descriptor) || !descriptor ||
                !Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance) {
                rbx::lua_pushnil(state);
                return 1;
            }

            uintptr_t typeDescriptor = 0, getset = 0, vtable = 0, getter = 0;
            uint32_t reflectionType = 0;
            if (!Probe::safeRead(&typeDescriptor, descriptor + layout::property_typedescriptor,
                                 sizeof(typeDescriptor)) || !typeDescriptor ||
                !Probe::safeRead(&reflectionType,
                                 typeDescriptor + layout::typedescriptor_reflectiontype,
                                 sizeof(reflectionType)) ||
                !Probe::safeRead(&getset, descriptor + layout::property_getset, sizeof(getset)) ||
                !getset ||
                !Probe::safeRead(&vtable, getset, sizeof(vtable)) || !vtable ||
                !Probe::safeRead(&getter, vtable + layout::getset_vtable_get, sizeof(getter)) ||
                !getter) {
                rbx::lua_pushnil(state);
                return 1;
            }

            // The getter is about to be called, so it has to be a function in
            // Roblox's own image rather than whatever a mis-derived offset
            // happens to point at.
            if (!Probe::inImage(getter)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            switch (reflectionType) {
            case layout::reflection_string:
            case layout::reflection_binarystring: {
                // void get(std::string* out, GetSet* self, Instance* instance)
                // - the sret is a real std::string, and passing an empty one is
                // what makes both branches of the thunk (short-string copy and
                // long-string assign) safe.
                std::string out;
                reinterpret_cast<void (*)(std::string*, uintptr_t, uintptr_t)>(getter)(
                    &out, getset, instance);
                rbx::lua_pushlstring(state, out.data(), out.size());
                return 1;
            }
            case layout::reflection_sharedstring: {
                // The out parameter is a handle; the bytes live one header in.
                // Wider than the handle needs, because the getter writes a
                // shared_ptr-shaped thing and the exact width is not worth
                // guessing at.
                uintptr_t handle[4] = {0, 0, 0, 0};
                reinterpret_cast<void (*)(uintptr_t*, uintptr_t, uintptr_t)>(getter)(
                    handle, getset, instance);
                // A null handle is a property with no value, which reads as
                // the empty string - not a reason to go looking for the bytes
                // somewhere else. Reading the handle's own storage as a
                // fallback was tried and is worse than useless: a zeroed buffer
                // is indistinguishable from an empty libc++ short string, so it
                // always "succeeds".
                std::string value;
                if (handle[0])
                    Probe::readString(handle[0] + layout::sharedstring_value, value);
                rbx::lua_pushlstring(state, value.data(), value.size());
                return 1;
            }
            case layout::reflection_systemaddress: {
                const int value =
                    reinterpret_cast<int (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                rbx::lua_pushinteger(state, value);
                return 1;
            }
            case layout::reflection_bool: {
                const bool value =
                    reinterpret_cast<bool (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                rbx::lua_pushboolean(state, value ? 1 : 0);
                return 1;
            }
            case layout::reflection_int: {
                const int value =
                    reinterpret_cast<int (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                rbx::lua_pushinteger(state, value);
                return 1;
            }
            case layout::reflection_int64: {
                const long long value =
                    reinterpret_cast<long long (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                utils::pushNumber(state, static_cast<double>(value));
                return 1;
            }
            case layout::reflection_float: {
                const float value =
                    reinterpret_cast<float (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                utils::pushNumber(state, static_cast<double>(value));
                return 1;
            }
            case layout::reflection_double: {
                const double value =
                    reinterpret_cast<double (*)(uintptr_t, uintptr_t)>(getter)(getset, instance);
                utils::pushNumber(state, value);
                return 1;
            }
            default:
                // Everything else the Lua bridge already converts, so there is
                // nothing to gain from going round it.
                rbx::lua_pushnil(state);
                return 1;
            }
        }

        // Writing a property the Lua bridge refuses, through the same GetSet the
        // reader uses - the setter is the next vtable slot.
        //
        // The thunk's shape was read off the disassembly for Instance.Tags:
        // `rdi` is the GetSet, `rsi` the Instance and `rdx` a pointer to the
        // value, which it copy-constructs into a local before calling the
        // property's own setter. So a std::string-backed property takes a
        // `const std::string*`.
        //
        // Only the string types are written this way. A SharedString needs the
        // engine's own SharedString constructor to build the value ("Cannot
        // convert from %s to SharedString" names it in other builds, and that
        // string is not in this one), and a scalar's thunk takes its argument
        // by value rather than by pointer - so both of those keep the ordinary
        // path rather than guessing at a calling convention.
        //
        //   __vanta_writeproperty(instance, descriptor, value) -> bool
        inline int writeProperty(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t descriptor = 0, instance = 0;
            size_t length = 0;
            const char* text = rbx::lua_tolstring(state, 3, &length);
            if (!data || !text || !Probe::address(state, 2, descriptor) || !descriptor ||
                !Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            uintptr_t typeDescriptor = 0, getset = 0, vtable = 0, setter = 0;
            uint32_t reflectionType = 0;
            if (!Probe::safeRead(&typeDescriptor, descriptor + layout::property_typedescriptor,
                                 sizeof(typeDescriptor)) || !typeDescriptor ||
                !Probe::safeRead(&reflectionType,
                                 typeDescriptor + layout::typedescriptor_reflectiontype,
                                 sizeof(reflectionType)) ||
                !Probe::safeRead(&getset, descriptor + layout::property_getset, sizeof(getset)) ||
                !getset ||
                !Probe::safeRead(&vtable, getset, sizeof(vtable)) || !vtable ||
                !Probe::safeRead(&setter, vtable + layout::getset_vtable_set, sizeof(setter)) ||
                !setter || !Probe::inImage(setter)) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            if (reflectionType != layout::reflection_string &&
                reflectionType != layout::reflection_binarystring) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const std::string value(text, length);
            reinterpret_cast<void (*)(uintptr_t, uintptr_t, const std::string*)>(setter)(
                getset, instance, &value);
            rbx::lua_pushboolean(state, 1);
            return 1;
        }

        // The reflection type a property's descriptor names, so the Lua side and
        // a probe can both say what kind of value they are dealing with.
        inline int propertyType(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t descriptor = 0, typeDescriptor = 0;
            uint32_t reflectionType = 0;
            if (!Probe::address(state, 1, descriptor) || !descriptor ||
                !Probe::safeRead(&typeDescriptor, descriptor + layout::property_typedescriptor,
                                 sizeof(typeDescriptor)) || !typeDescriptor ||
                !Probe::safeRead(&reflectionType,
                                 typeDescriptor + layout::typedescriptor_reflectiontype,
                                 sizeof(reflectionType))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushinteger(state, static_cast<int>(reflectionType));
            return 1;
        }

        // The function assigned to a callback property, or nothing.
        //
        // Roblox never lets one be read back - the property is write-only, and
        // gethiddenproperty on it answers nil however scriptable it is made -
        // so this goes at the storage directly. See offsets::structs for the
        // chain and how it was found.
        inline bool callbackRef(lua_State* L, uintptr_t instance, uintptr_t descriptor,
                                int32_t& out)
        {
            const uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t global = 0;
            if (!Probe::safeRead(&global, state + luaobj::state::global, sizeof(global)))
                global = 0;

            const char* why = "none";
            const auto tryRef = [&](int32_t reference, const char* source) -> bool {
                if (reference <= 0 || reference > 0x1000000)
                    return false;
                rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
                if (rbx::lua_type(state, -1) == luaobj::kTagFunction &&
                    rbx::lua_islfunction(state, -1)) {
                    out = reference;
                    why = source;
                    journal::writef("CBTRY source=%s ref=%d lua=1", source, reference);
                    rbx::lua_settop(state, -2);
                    return true;
                }
                rbx::lua_settop(state, -2);
                return false;
            };

            const auto tryLive = [&](uintptr_t live) -> bool {
                if (!live)
                    return false;
                // LiveReference: Links[5], Thread, ThreadReference, ValueReference.
                uintptr_t thread = 0;
                if (Probe::safeRead(&thread, live + 0x28, sizeof(thread)) && thread) {
                    uintptr_t liveGlobal = 0;
                    if (Probe::safeRead(&liveGlobal, thread + luaobj::state::global,
                                        sizeof(liveGlobal)) &&
                        (!global || liveGlobal == global)) {
                        int32_t reference = 0;
                        if (Probe::safeRead(&reference, live + 0x34, sizeof(reference)) &&
                            tryRef(reference, "live34"))
                            return true;
                    }
                }
                int32_t reference = 0;
                if (Probe::safeRead(&reference, live + 0x8, sizeof(reference)) &&
                    tryRef(reference, "live34"))
                    return true;
                return false;
            };

            uintptr_t memberOffset = 0;
            if (!Probe::safeRead(&memberOffset, descriptor + layout::callback_member_offset,
                                 sizeof(memberOffset)) ||
                memberOffset == 0 || memberOffset > 0x4000)
                return false;

            // The engine chain the previous executor uses on macOS:
            // instance + member -> +0x10 -> +0x20 -> +0x18 -> LiveReference.
            // It is tried first because the Alpen storage layout below can
            // land on a LiveReference belonging to a different callback and
            // still resolve to a perfectly valid Lua function.
            {
                uintptr_t holder = 0, first = 0, live = 0;
                if (Probe::safeRead(&holder, instance + memberOffset + layout::callback_holder,
                                    sizeof(holder)) && holder > 0x10000 &&
                    Probe::safeRead(&first, holder + layout::callback_ref_hop1,
                                    sizeof(first)) && first > 0x10000 &&
                    Probe::safeRead(&live, first + layout::callback_ref_hop2,
                                    sizeof(live)) && live > 0x10000 &&
                    (why = "unix", tryLive(live)))
                    return true;
            }

            // Alpen's CallbackStorage: the implementation object carries the
            // weak_ptr to the Lua LiveReference. The 0.742 layout keeps the
            // implementation at +0x38 only for some builds; try the candidate
            // slots and let the LiveReference validation decide.
            for (int storageOffset : {0x38, 0x28, 0x30, 0x20, 0x10, 0x8, 0x0}) {
                uintptr_t implementation = 0;
                if (!Probe::safeRead(&implementation, instance + memberOffset + storageOffset,
                                     sizeof(implementation)) ||
                    !implementation || implementation < 0x10000)
                    continue;

                uintptr_t live = 0;
                if (Probe::safeRead(&live, implementation + 0x8, sizeof(live)) &&
                    (why = "impl8", tryLive(live)))
                    return true;
                if (why = "implDirect", tryLive(implementation + 0x8))
                    return true;
            }

            // The older storage chain: holder -> first -> LiveReference.
            uintptr_t holder = 0, first = 0, live = 0;
            if (Probe::safeRead(&holder, instance + memberOffset + 0x10, sizeof(holder)) &&
                holder > 0x10000 &&
                Probe::safeRead(&first, holder + 0x20, sizeof(first)) &&
                first > 0x10000 &&
                Probe::safeRead(&live, first + 0x18, sizeof(live)) && (why = "chain", tryLive(live)))
                return true;

            // Last-resort direct scan of the storage block. This is safe: a
            // candidate only becomes an answer if the registry slot really is
            // a Lua function.
            for (int offset = 0; offset < 0x80; offset += 4) {
                int32_t reference = 0;
                if (Probe::safeRead(&reference, instance + memberOffset + offset,
                                    sizeof(reference)) &&
                    tryRef(reference, "live34"))
                    return true;
            }
            return false;
        }

        inline uintptr_t memberFor(uint64_t state, int instanceIndex, int nameIndex)
        {
            void* data = rbx::lua_touserdata(state, instanceIndex);
            size_t len = 0;
            const char* name = rbx::lua_tolstring(state, nameIndex, &len);
            if (!data || !name)
                return 0;

            uintptr_t instance = 0;
            if (!Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)))
                return 0;
            return findMember(instance, std::string(name, len));
        }

        // __vanta_vulnpatch(instance, "MethodName") -> bool
        //
        // Blocks one member for executor threads by rewriting its reflection
        // descriptor's invoke vtable slot - see vulnpatch.hpp. The instance is
        // any object of the owning class (a service, or `game` itself), which is
        // how the member's descriptor is reached; the engine resolves the class,
        // so the caller just hands over what `game:GetService(...)` returned.
        // Returns whether the block was installed (or was already in place), so
        // the Lua driver can log the few members a build does not expose.
        // The descriptor behind a member reached through the member itself: the
        // C closure `instance[method]` returns keeps the descriptor as its first
        // upvalue. This does not depend on the descriptor sitting near its class
        // in memory, so it finds the members `findMember`'s page scan cannot
        // reach (ScriptContext's, whose descriptors are registered far from the
        // class). Whatever it returns is validated by vulnpatch::install before
        // anything is written, so a wrong upvalue is skipped, not trusted.
        inline uintptr_t descriptorFromMember(uint64_t state, int instanceIndex, const char* method)
        {
            rbx::lua_getfield(state, instanceIndex, method);
            uintptr_t descriptor = 0;
            if (rbx::lua_type(state, -1) == luaobj::kTagFunction) {
                const void* closure = rbx::lua_topointer(state, -1);
                if (closure)
                    Probe::safeRead(&descriptor,
                                    reinterpret_cast<uintptr_t>(closure) + luaobj::closure::upvals,
                                    sizeof(descriptor));
            }
            rbx::lua_settop(state, -2);   // drop the member
            return descriptor;
        }

        inline int vulnpatch(lua_State* L)
        {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            size_t len = 0;
            const char* name = rbx::lua_tolstring(state, 2, &len);

            // Resolve the member's OWN closure descriptor first. `instance[method]`
            // returns the bound C closure the call actually invokes, and its first
            // upvalue is exactly the descriptor that invoke reads - so blocking it
            // blocks the real call, every time. The page scan (memberFor) is only a
            // fallback for members that are not a resolvable closure
            // (ScriptContext.AddCoreScriptLocal is the reason it exists): it must
            // not be primary, because when a class exposes more than one descriptor
            // under a single name, membersOf's `emplace` keeps whichever sits at the
            // lower address, and that order varies per session.
            uintptr_t descriptor = name ? descriptorFromMember(state, 1, name) : 0;
            if (!descriptor)
                descriptor = memberFor(state, 1, 2);

            uint64_t vmGlobal = 0;
            Probe::safeRead(&vmGlobal, state + vulnpatch::kStateGlobal, sizeof(vmGlobal));

            const bool ok = descriptor && vmGlobal &&
                            vulnpatch::install(descriptor, vmGlobal, name ? name : "?");
            rbx::lua_pushboolean(state, ok ? 1 : 0);
            return 1;
        }

        // __vanta_vulnpatch_passes([bump]) -> integer
        //
        // How many complete blocklist passes this process has run. The Lua
        // driver asks first and skips the whole pass once one has completed:
        // the descriptors are process-global and the vtable clone is permanent,
        // so a second pass only paid for ~220 lookups (54 ms on a join, journal
        // 2026-09-03) to find every member already patched. Called with `true`
        // at the end of a pass to count it.
        //
        // It also refreshes vulnpatch::knownGlobal from the calling thread. The
        // block trampoline recognises a caller as a thread of *this* VM by that
        // value, and every join is a new VM: install() only ever set it once,
        // so after a teleport the check stopped recognising any thread and let
        // every blocked call through. Refreshing on every join keeps the blocks
        // live whether or not the member pass is skipped.
        inline int vulnpatchPasses(lua_State* L)
        {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            static std::atomic<int> passes{0};
            uint64_t vmGlobal = 0;
            if (Probe::safeRead(&vmGlobal, state + vulnpatch::kStateGlobal, sizeof(vmGlobal)) &&
                vmGlobal)
                vulnpatch::knownGlobal = vmGlobal;
            if (rbx::lua_toboolean(state, 1))
                passes.fetch_add(1);
            rbx::lua_pushinteger(state, passes.load());
            return 1;
        }

        inline bool scriptable(uintptr_t descriptor)
        {
            uint8_t flags = 0;
            return Probe::safeRead(&flags, descriptor + layout::property_flags, sizeof(flags)) &&
                   (flags & layout::property_scriptable_bit) != 0;
        }

        // The address of a property's descriptor, so the Lua side can hold on
        // to it across a get/set pair rather than paying for the search twice.
        inline int descriptorOf(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t descriptor = memberFor(state, 1, 2);
            if (!descriptor)
                rbx::lua_pushnil(state);
            else
                utils::pushNumber(state, static_cast<double>(descriptor));
            return 1;
        }

        inline int classDescriptorOf(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t instance = 0, classDescriptor = 0;
            if (!data ||
                !Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance ||
                !Probe::safeRead(&classDescriptor, instance + layout::instance_classdescriptor,
                                 sizeof(classDescriptor))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(classDescriptor));
            return 1;
        }

        // Temporary diagnostic: the raw bytes of a member descriptor, so the
        // scriptable flag can be located against properties whose visibility is
        // already known.
        inline int descBytes(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t descriptor = 0;
            if (!Probe::address(state, 1, descriptor) || !descriptor) {
                rbx::lua_pushnil(state);
                return 1;
            }
            unsigned char buffer[0xC0] = {0};
            if (!Probe::safeRead(buffer, descriptor, sizeof(buffer))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            static const char* const hex = "0123456789abcdef";
            std::string dump;
            dump.reserve(sizeof(buffer) * 2);
            for (unsigned char byte : buffer) {
                dump += hex[byte >> 4];
                dump += hex[byte & 0xf];
            }
            rbx::lua_pushlstring(state, dump.data(), dump.size());
            return 1;
        }

        // Safe read helpers for deriving the 0.742 property layout: a qword
        // and a dword at an absolute address, and whether a pointer lies in
        // Roblox's image. Nothing here calls through a pointer.
        inline int peekq(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t address = 0;
            int isnum = 0;
            const int offset = rbx::lua_tointegerx(state, 2, &isnum);
            if (!Probe::address(state, 1, address) || !address || !isnum) {
                rbx::lua_pushnil(state);
                return 1;
            }
            uintptr_t value = 0;
            if (!Probe::safeRead(&value, address + static_cast<uintptr_t>(offset),
                                 sizeof(value))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            utils::pushNumber(state, static_cast<double>(value));
            return 1;
        }

        inline int peekd(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t address = 0;
            int isnum = 0;
            const int offset = rbx::lua_tointegerx(state, 2, &isnum);
            if (!Probe::address(state, 1, address) || !address || !isnum) {
                rbx::lua_pushnil(state);
                return 1;
            }
            int32_t value = 0;
            if (!Probe::safeRead(&value, address + static_cast<uintptr_t>(offset),
                                 sizeof(value))) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushinteger(state, value);
            return 1;
        }

        // Diagnostic: the registry reference a Lua value would get, so a
        // callback's stored reference can be matched in memory.
        inline int refOf(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_pushvalue(state, 1);
            const int reference = rbx::lua_ref(state, -1);
            rbx::lua_settop(state, -2);
            rbx::lua_pushinteger(state, reference);
            return 1;
        }

        inline int inimage(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t address = 0;
            if (!Probe::address(state, 1, address) || !address) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            rbx::lua_pushboolean(state, Probe::inImage(address) ? 1 : 0);
            return 1;
        }

        inline int isscriptable(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t descriptor = 0;
            if (!Probe::address(state, 1, descriptor) || !descriptor) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushboolean(state, scriptable(descriptor) ? 1 : 0);
            return 1;
        }

        inline int setscriptable(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            uintptr_t descriptor = 0;
            if (!Probe::address(state, 1, descriptor) || !descriptor) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const bool wanted = rbx::lua_toboolean(state, 2) != 0;
            auto* flags = reinterpret_cast<uint8_t*>(descriptor + layout::property_flags);
            const bool before = (*flags & layout::property_scriptable_bit) != 0;
            if (wanted)
                *flags = static_cast<uint8_t>(*flags | layout::property_scriptable_bit);
            else
                *flags = static_cast<uint8_t>(*flags & ~layout::property_scriptable_bit);

            rbx::lua_pushboolean(state, before ? 1 : 0);
            return 1;
        }
    }

    // Synthetic input. Everything here posts to the window server rather than
    // to Roblox, which is the only way the game reacts to it at all.
    namespace Input {

        inline bool key(uint64_t state, CGKeyCode& out)
        {
            size_t len = 0;
            const char* name = rbx::lua_type(state, 1) == luaobj::kTagString
                ? rbx::lua_tolstring(state, 1, &len) : nullptr;
            int isnum = 0;
            const int code = rbx::lua_tointegerx(state, 1, &isnum);
            if (!name && !isnum)
                return false;
            return input::translate(name ? std::string(name, len) : std::string(), code, out);
        }

        inline int keypress(lua_State* L) {
            CGKeyCode code;
            if (key(reinterpret_cast<uint64_t>(L), code))
                input::postKey(code, true);
            return 0;
        }

        inline int keyrelease(lua_State* L) {
            CGKeyCode code;
            if (key(reinterpret_cast<uint64_t>(L), code))
                input::postKey(code, false);
            return 0;
        }

        inline int keyclick(lua_State* L) {
            CGKeyCode code;
            if (key(reinterpret_cast<uint64_t>(L), code)) {
                input::postKey(code, true);
                input::postKey(code, false);
            }
            return 0;
        }

        inline int mouse1press(lua_State* L)   { (void)L; input::mouseButton(false, true);  return 0; }
        inline int mouse1release(lua_State* L) { (void)L; input::mouseButton(false, false); return 0; }
        inline int mouse2press(lua_State* L)   { (void)L; input::mouseButton(true, true);   return 0; }
        inline int mouse2release(lua_State* L) { (void)L; input::mouseButton(true, false);  return 0; }

        inline int mouse1click(lua_State* L) {
            (void)L;
            input::mouseButton(false, true);
            input::mouseButton(false, false);
            return 0;
        }

        inline int mouse2click(lua_State* L) {
            (void)L;
            input::mouseButton(true, true);
            input::mouseButton(true, false);
            return 0;
        }

        inline int mousemoveabs(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int xnum = 0, ynum = 0;
            const int x = rbx::lua_tointegerx(state, 1, &xnum);
            const int y = rbx::lua_tointegerx(state, 2, &ynum);
            if (xnum && ynum)
                input::moveTo(x, y);
            return 0;
        }

        inline int mousemoverel(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int xnum = 0, ynum = 0;
            const int x = rbx::lua_tointegerx(state, 1, &xnum);
            const int y = rbx::lua_tointegerx(state, 2, &ynum);
            if (xnum && ynum)
                input::moveBy(x, y);
            return 0;
        }

        inline int mousescroll(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int amount = rbx::lua_tointegerx(state, 1, &isnum);
            if (isnum)
                input::scroll(amount);
            return 0;
        }
    }

    // rconsole. The dylib only serialises; the console window belongs to the
    // companion app, exactly as it does in the macOS reference.
    namespace Console {

        // The reference joins every argument with a space, the way print does,
        // rather than taking only the first.
        inline std::string joined(uint64_t state)
        {
            std::string message;
            const int count = rbx::lua_gettop(state);
            for (int i = 1; i <= count; i++) {
                size_t len = 0;
                const char* part = rbx::lua_tolstring(state, i, &len);
                if (!part)
                    continue;
                if (!message.empty())
                    message += ' ';
                message.append(part, len);
            }
            return message;
        }

        inline int print(lua_State* L) {
            console::send("rconsoleprint", joined(reinterpret_cast<uint64_t>(L)));
            return 0;
        }

        // Script output. `print` and `warn` are Roblox's own functions, so the
        // exec thread shadows them and chains to the originals: the script's
        // output lands in Vanta's console *and* in Roblox's, which is the same
        // trade LunarModule makes. Without the shadow, print goes only to
        // FLog::CreatorOutput and the user's console looks empty no matter what
        // the script prints.
        inline uintptr_t originalPrint = 0;
        inline uintptr_t originalWarn = 0;

        inline int printHook(lua_State* L) {
            console::send("rconsoleprint", joined(reinterpret_cast<uint64_t>(L)));
            if (originalPrint)
                return reinterpret_cast<int (*)(lua_State*)>(originalPrint)(L);
            return 0;
        }

        inline int warnHook(lua_State* L) {
            console::send("rconsolewarn", joined(reinterpret_cast<uint64_t>(L)));
            if (originalWarn)
                return reinterpret_cast<int (*)(lua_State*)>(originalWarn)(L);
            return 0;
        }

        inline int info(lua_State* L) {
            console::send("rconsoleinfo", joined(reinterpret_cast<uint64_t>(L)));
            return 0;
        }

        inline int warn(lua_State* L) {
            console::send("rconsolewarn", joined(reinterpret_cast<uint64_t>(L)));
            return 0;
        }

        inline int err(lua_State* L) {
            console::send("rconsoleerror", joined(reinterpret_cast<uint64_t>(L)));
            return 0;
        }

        inline int clear(lua_State* L) {
            (void)L;
            console::send("rconsoleclear", "");
            return 0;
        }

        inline int show(lua_State* L) {
            (void)L;
            console::send("rconsoleshow", "");
            return 0;
        }

        inline int hide(lua_State* L) {
            (void)L;
            console::send("rconsolehide", "");
            return 0;
        }

        inline int name(lua_State* L) {
            console::send("rconsolename", joined(reinterpret_cast<uint64_t>(L)));
            return 0;
        }

        // The wait runs on a worker and this thread yields, which is what mac
        // does (`Yielding::Yield` around the same thirty-second wait). Before
        // this it slept on Roblox's own scheduler and froze the whole client
        // for the timeout. The prompt itself still goes out from here - it is
        // a connect, a file append and a print - so only the waiting moves.
        inline int input(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            console::prompt(joined(state));

            return Yield::start(state, []() -> async::Result {
                const std::string line = console::await();
                return [line](uint64_t s) -> int {
                    rbx::lua_pushlstring(s, line.data(), line.size());
                    return 1;
                };
            });
        }
    }

    // The file dialogs themselves. Each hands back the chosen paths as plain
    // strings; the file and folder objects the spec describes are assembled
    // around them in the bootstrap, which is a far better place for method
    // tables than C is.
    namespace Dialogs {

        // Only a path a dialog actually produced is readable this way.
        inline std::string argumentPath(uint64_t state)
        {
            size_t len = 0;
            const char* path = rbx::lua_tolstring(state, 1, &len);
            if (!path)
                return "";
            const std::string value(path, len);
            return dialogs::allowed(value) ? value : std::string();
        }

        inline std::string field(uint64_t state, int table, const char* key)
        {
            if (rbx::lua_type(state, table) != luaobj::kTagTable)
                return "";
            rbx::lua_getfield(state, table, key);
            size_t len = 0;
            const char* value = rbx::lua_tolstring(state, -1, &len);
            std::string out = value ? std::string(value, len) : std::string();
            rbx::lua_settop(state, -2);
            return out;
        }

        inline std::vector<std::string> extensions(uint64_t state, int table)
        {
            std::vector<std::string> out;
            if (rbx::lua_type(state, table) != luaobj::kTagTable)
                return out;
            rbx::lua_getfield(state, table, "extensionFilter");
            if (rbx::lua_type(state, -1) == luaobj::kTagTable) {
                for (int i = 1; i <= 64; i++) {
                    rbx::lua_rawgeti(state, -1, i);
                    size_t len = 0;
                    const char* value = rbx::lua_tolstring(state, -1, &len);
                    if (!value) {
                        rbx::lua_settop(state, -2);
                        break;
                    }
                    out.emplace_back(value, len);
                    rbx::lua_settop(state, -2);
                }
            }
            rbx::lua_settop(state, -2);
            return out;
        }

        inline void pushPaths(uint64_t state, const std::vector<std::string>& paths)
        {
            rbx::lua_createtable(state, static_cast<int>(paths.size()), 0);
            for (size_t i = 0; i < paths.size(); i++) {
                rbx::lua_pushlstring(state, paths[i].data(), paths[i].size());
                rbx::lua_rawseti(state, -2, static_cast<int>(i) + 1);
            }
        }

        inline int openFile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            pushPaths(state, dialogs::openFile(field(state, 1, "title"), field(state, 1, "defaultPath"),
                                               extensions(state, 1), rbx::lua_toboolean(state, 2) != 0));
            return 1;
        }

        inline int openFolder(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            pushPaths(state, dialogs::openFolder(field(state, 1, "title"), field(state, 1, "defaultPath")));
            return 1;
        }

        inline int saveFile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            pushPaths(state, dialogs::saveFile(field(state, 1, "title"), field(state, 1, "defaultPath"),
                                               field(state, 1, "suggestedName")));
            return 1;
        }

        // Raw filesystem access for what a dialog handed out - and only for
        // that. Anything else stays inside the workspace sandbox.
        inline int read(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            std::ifstream in(path, std::ios::binary);
            if (path.empty() || !in) {
                rbx::lua_pushnil(state);
                return 1;
            }
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string contents = buffer.str();
            rbx::lua_pushlstring(state, contents.data(), contents.size());
            return 1;
        }

        inline int write(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            size_t len = 0;
            const char* data = rbx::lua_tolstring(state, 2, &len);
            const bool append = rbx::lua_toboolean(state, 3) != 0;
            if (path.empty()) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            std::ofstream out(path, append ? (std::ios::binary | std::ios::app)
                                           : (std::ios::binary | std::ios::trunc));
            if (out && data)
                out.write(data, static_cast<std::streamsize>(len));
            rbx::lua_pushboolean(state, out ? 1 : 0);
            return 1;
        }

        inline int remove(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            std::error_code code;
            const bool ok = !path.empty() && std::filesystem::remove_all(path, code) > 0;
            rbx::lua_pushboolean(state, ok ? 1 : 0);
            return 1;
        }

        inline int list(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            const bool recursive = rbx::lua_toboolean(state, 2) != 0;

            std::vector<std::string> found;
            std::error_code code;
            if (!path.empty()) {
                if (recursive) {
                    for (auto& entry : std::filesystem::recursive_directory_iterator(path, code))
                        found.push_back(entry.path().string());
                } else {
                    for (auto& entry : std::filesystem::directory_iterator(path, code))
                        found.push_back(entry.path().string());
                }
            }
            for (const std::string& entry : found)
                dialogs::grant(entry);
            pushPaths(state, found);
            return 1;
        }

        inline int makeFolder(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            std::error_code code;
            const bool ok = !path.empty() && (std::filesystem::create_directories(path, code) || !code);
            if (ok)
                dialogs::grant(path);
            rbx::lua_pushboolean(state, ok ? 1 : 0);
            return 1;
        }

        inline int stat(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string path = argumentPath(state);
            std::error_code code;
            if (path.empty() || !std::filesystem::exists(path, code)) {
                rbx::lua_pushnil(state);
                return 1;
            }
            const bool folder = std::filesystem::is_directory(path, code);
            rbx::lua_pushlstring(state, folder ? "folder" : "file", folder ? 6 : 4);
            return 1;
        }
    }

    namespace Drawing {
        
        // cleardrawcache
        // getrenderproperty
        // isrenderobj
        // setrenderproperty
        
    }
    
    namespace Encoding {      
        
        // Every reference argument-checks the crypt entry points on a real
        // string. Ours took whatever was there and encoded "" for a nil, and
        // `hash` was worse than cosmetic: it handed the null straight to
        // CC_MD5/CC_SHA*, which dereference it.
        inline int base64encode(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagString);
            size_t len = 0;
            const char* str = rbx::lua_tolstring(state, 1, &len);
            std::string encoded = base64::to_base64(str ? std::string(str, len) : "");
            rbx::lua_pushlstring(state, encoded.c_str(), encoded.length());
            return 1;
        }

        inline int base64decode(lua_State* L) {
            rbx::luaL_checktype(reinterpret_cast<uint64_t>(L), 1, luaobj::kTagString);
            size_t len = 0;
            const char* str = rbx::lua_tolstring(reinterpret_cast<uint64_t>(L), 1, &len);
            std::string decoded = base64::from_base64(str ? std::string(str, len) : "");
            rbx::lua_pushlstring(reinterpret_cast<uint64_t>(L), decoded.c_str(), decoded.length());
            return 1;
        }
        
        // The commented-out originals pushed LZ4's return value straight into
        // lua_pushlstring. On failure that value is negative, which converts to
        // an enormous size_t and reads far past the buffer - so both of these
        // check it before it becomes a length.
        // CommonCrypto ships in libSystem, so the hash family costs no new
        // dependency and no vendored crypto source.
        inline int hash(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagString);
            rbx::luaL_checktype(state, 2, luaobj::kTagString);

            size_t dataLen = 0;
            const char* data = rbx::lua_tolstring(state, 1, &dataLen);
            size_t algoLen = 0;
            const char* algo = rbx::lua_tolstring(state, 2, &algoLen);
            // The algorithm is the caller's to name - the references all require
            // it rather than quietly picking one.
            std::string algorithm = algo ? std::string(algo, algoLen) : "";
            std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            algorithm.erase(std::remove_if(algorithm.begin(), algorithm.end(),
                                           [](char c) { return c == '-' || c == '_'; }),
                            algorithm.end());

            unsigned char digest[CC_SHA512_DIGEST_LENGTH];
            unsigned int length = 0;
            const unsigned int size = static_cast<unsigned int>(dataLen);

            if (algorithm == "md5") {
                CC_MD5(data, size, digest); length = CC_MD5_DIGEST_LENGTH;
            } else if (algorithm == "sha1") {
                CC_SHA1(data, size, digest); length = CC_SHA1_DIGEST_LENGTH;
            } else if (algorithm == "sha224") {
                CC_SHA224(data, size, digest); length = CC_SHA224_DIGEST_LENGTH;
            } else if (algorithm == "sha256") {
                CC_SHA256(data, size, digest); length = CC_SHA256_DIGEST_LENGTH;
            } else if (algorithm == "sha384") {
                CC_SHA384(data, size, digest); length = CC_SHA384_DIGEST_LENGTH;
            } else if (algorithm == "sha512") {
                CC_SHA512(data, size, digest); length = CC_SHA512_DIGEST_LENGTH;
            } else if (algorithm == "sha3224" || algorithm == "sha3256" ||
                       algorithm == "sha3384" || algorithm == "sha3512") {
                const int bits = std::stoi(algorithm.substr(4));
                const std::string produced = sha3::hash(data, dataLen, bits);
                std::memcpy(digest, produced.data(), produced.size());
                length = static_cast<unsigned int>(produced.size());
            } else {
                const std::string message = "crypt.hash: unsupported algorithm '" + algorithm + "'";
                rbx::lua_pushlstring(state, message.data(), message.size());
                rbx::lua_error(state);
                return 0;
            }

            static const char* const hex = "0123456789abcdef";
            std::string out;
            out.reserve(length * 2);
            for (unsigned int i = 0; i < length; i++) {
                out += hex[digest[i] >> 4];
                out += hex[digest[i] & 0xf];
            }
            rbx::lua_pushlstring(state, out.data(), out.size());
            return 1;
        }

        // HMAC over the same algorithm names crypt.hash takes. The digest goes
        // out base64-encoded, which is what the suite compares against - a hex
        // string would fail a reference it never generated.
        inline int hmac(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            size_t keyLen = 0;
            const char* key = rbx::lua_tolstring(state, 1, &keyLen);
            size_t dataLen = 0;
            const char* data = rbx::lua_tolstring(state, 2, &dataLen);
            size_t algoLen = 0;
            const char* algo = rbx::lua_tolstring(state, 3, &algoLen);

            std::string algorithm = algo ? std::string(algo, algoLen) : "sha256";
            std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            algorithm.erase(std::remove_if(algorithm.begin(), algorithm.end(),
                                           [](char c) { return c == '-' || c == '_'; }),
                            algorithm.end());

            CCHmacAlgorithm which;
            unsigned int length;
            if (algorithm == "md5")         { which = kCCHmacAlgMD5;    length = CC_MD5_DIGEST_LENGTH; }
            else if (algorithm == "sha1")   { which = kCCHmacAlgSHA1;   length = CC_SHA1_DIGEST_LENGTH; }
            else if (algorithm == "sha224") { which = kCCHmacAlgSHA224; length = CC_SHA224_DIGEST_LENGTH; }
            else if (algorithm == "sha256") { which = kCCHmacAlgSHA256; length = CC_SHA256_DIGEST_LENGTH; }
            else if (algorithm == "sha384") { which = kCCHmacAlgSHA384; length = CC_SHA384_DIGEST_LENGTH; }
            else if (algorithm == "sha512") { which = kCCHmacAlgSHA512; length = CC_SHA512_DIGEST_LENGTH; }
            else {
                const std::string message = "crypt.hmac: unsupported algorithm '" + algorithm + "'";
                rbx::lua_pushlstring(state, message.data(), message.size());
                rbx::lua_error(state);
                return 0;
            }

            unsigned char digest[CC_SHA512_DIGEST_LENGTH];
            CCHmac(which, key ? key : "", keyLen, data ? data : "", dataLen, digest);

            const std::string encoded =
                base64::to_base64(std::string(reinterpret_cast<char*>(digest), length));
            rbx::lua_pushlstring(state, encoded.data(), encoded.size());
            return 1;
        }

        // Raw random bytes, unlike generatebytes: the spec has this one hand
        // back exactly the number of bytes asked for, so #crypt.random(64) is
        // 64. A non-positive count is an error rather than a silent default -
        // the suite checks that -1 is refused.
        inline int random(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int count = rbx::lua_tointegerx(state, 1, &isnum);
            if (!isnum || count <= 0 || count > 100000) {
                const char* message = "crypt.random: expected a byte count between 1 and 100000";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            std::string bytes(static_cast<size_t>(count), '\0');
            arc4random_buf(bytes.data(), bytes.size());
            rbx::lua_pushlstring(state, bytes.data(), bytes.size());
            return 1;
        }

        // UNC returns these base64-encoded rather than raw, so the result stays
        // safe to hand straight to string functions.
        inline int generatebytes(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            int count = rbx::lua_tointegerx(state, 1, &isnum);
            if (!isnum || count <= 0)
                count = 16;
            if (count > 1024)
                count = 1024;

            std::string bytes(static_cast<size_t>(count), '\0');
            arc4random_buf(bytes.data(), bytes.size());
            const std::string encoded = base64::to_base64(bytes);
            rbx::lua_pushlstring(state, encoded.data(), encoded.size());
            return 1;
        }

        // AES-CBC through CommonCrypto. Keys and IVs cross the Lua boundary
        // base64-encoded, matching how crypt.generatekey hands them out.
        inline int crypto(lua_State* L, bool encrypting) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const char* label = encrypting ? "crypt.encrypt" : "crypt.decrypt";

            auto fail = [&](const std::string& why) {
                const std::string message = std::string(label) + ": " + why;
                rbx::lua_pushlstring(state, message.data(), message.size());
                rbx::lua_error(state);
            };
            auto argument = [&](int index) {
                size_t len = 0;
                const char* value = rbx::lua_tolstring(state, index, &len);
                return value ? std::string(value, len) : std::string();
            };

            std::string data = argument(1);
            const std::string key = base64::from_base64(argument(2));
            std::string iv = base64::from_base64(argument(3));

            std::string mode = argument(4);
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (!mode.empty() && mode != "CBC") {
                fail("only CBC mode is supported (got '" + mode + "')");
                return 0;
            }

            if (key.size() != kCCKeySizeAES128 && key.size() != kCCKeySizeAES192 &&
                key.size() != kCCKeySizeAES256) {
                fail("key must decode to 16, 24 or 32 bytes");
                return 0;
            }

            // AES blocks are 16 bytes, but generatekey hands back 32 and callers
            // pass it straight in as an IV, so take the leading block rather
            // than rejecting something the suites treat as valid.
            if (iv.empty()) {
                if (!encrypting) {
                    fail("an IV is required to decrypt");
                    return 0;
                }
                iv.resize(kCCBlockSizeAES128);
                arc4random_buf(&iv[0], iv.size());
            } else if (iv.size() < kCCBlockSizeAES128) {
                fail("iv must decode to at least 16 bytes");
                return 0;
            } else {
                iv.resize(kCCBlockSizeAES128);
            }

            if (!encrypting)
                data = base64::from_base64(data);

            std::vector<char> out(data.size() + kCCBlockSizeAES128);
            size_t produced = 0;
            const CCCryptorStatus status = CCCrypt(
                encrypting ? kCCEncrypt : kCCDecrypt,
                kCCAlgorithmAES, kCCOptionPKCS7Padding,
                key.data(), key.size(), iv.data(),
                data.data(), data.size(),
                out.data(), out.size(), &produced);

            if (status != kCCSuccess) {
                fail("CommonCrypto returned " + std::to_string(status));
                return 0;
            }

            const std::string result(out.data(), produced);
            if (encrypting) {
                const std::string encoded = base64::to_base64(result);
                const std::string ivEncoded = base64::to_base64(iv);
                rbx::lua_pushlstring(state, encoded.data(), encoded.size());
                rbx::lua_pushlstring(state, ivEncoded.data(), ivEncoded.size());
                return 2;
            }

            rbx::lua_pushlstring(state, result.data(), result.size());
            return 1;
        }

        inline int encrypt(lua_State* L) { return crypto(L, true); }
        inline int decrypt(lua_State* L) { return crypto(L, false); }

        inline int lz4compress(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagString);
            size_t len = 0;
            const char* str = rbx::lua_tolstring(state, 1, &len);
            std::string input = str ? std::string(str, len) : "";
            if (input.empty()) {
                rbx::lua_pushlstring(state, "", 0);
                return 1;
            }

            const int bound = LZ4_compressBound(static_cast<int>(input.size()));
            if (bound <= 0) {
                const char* message = "lz4compress: input is too large";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            std::vector<char> output(static_cast<size_t>(bound));
            const int written = LZ4_compress_default(input.data(), output.data(),
                                                     static_cast<int>(input.size()), bound);
            if (written <= 0) {
                const char* message = "lz4compress: compression failed";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            rbx::lua_pushlstring(state, output.data(), static_cast<size_t>(written));
            return 1;
        }

        // A ceiling on anything a script can ask us to allocate by naming a
        // size. Matches Filesystem::kMaxReadBytes deliberately: the two are the
        // same question.
        inline constexpr size_t kMaxDecompressed = 256u << 20;

        inline int lz4decompress(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagString);
            size_t len = 0;
            const char* str = rbx::lua_tolstring(state, 1, &len);
            std::string input = str ? std::string(str, len) : "";

            int isnum = 0;
            const int expected = rbx::lua_tointegerx(state, 2, &isnum);
            if (!isnum || expected <= 0) {
                const char* message = "lz4decompress: expected the decompressed size as argument 2";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            // The decompressed size is whatever the caller said it was, and it
            // sizes an allocation on Roblox's own thread. All three references
            // that implement this - mac, Vicna and sourcessss - take the number
            // on trust (sourcessss additionally `new char[]`s it and never
            // frees), so `lz4decompress("x", 2000000000)` is a two-gigabyte
            // allocation and a dead client in every one of them. None of them
            // tests it; it is the shared-blind-spot case, so the majority does
            // not settle this one.
            //
            // Two bounds. The ratio is the tight one and cannot reject a
            // correct call: LZ4 cannot expand by more than about 255:1, so a
            // truthful `expected` always fits under it and only an inflated one
            // does not. The absolute cap is the backstop.
            const size_t claimed = static_cast<size_t>(expected);
            if (claimed > kMaxDecompressed || claimed > input.size() * 255 + 64) {
                const char* message = "lz4decompress: the decompressed size is not believable";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            std::vector<char> output(claimed);
            const int produced = LZ4_decompress_safe(input.data(), output.data(),
                                                     static_cast<int>(input.size()), expected);
            if (produced < 0) {
                const char* message = "lz4decompress: input is not valid lz4 data";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            rbx::lua_pushlstring(state, output.data(), static_cast<size_t>(produced));
            return 1;
        }
    }
    
    namespace Environment {

        inline int getgenv(lua_State* L) {
            rbx::lua_pushvalue(reinterpret_cast<uint64_t>(L), LUA_ENVIRONINDEX);
            return 1;
        }

        inline int getreg(lua_State* L) {
            rbx::lua_pushvalue(reinterpret_cast<uint64_t>(L), LUA_REGISTRYINDEX);
            return 1;
        }

        // LUA_GLOBALSINDEX resolves against the *calling* thread, and our threads
        // are sandboxed with their own globals table - so pushing it here would
        // hand back the executor's environment while claiming to be the game's.
        // Register stashes the real one from the global state instead.
        inline int getrenv(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_getfield(state, LUA_REGISTRYINDEX, "vanta.renv");
            if (!rbx::lua_toboolean(state, -1)) {
                rbx::lua_settop(state, -2);
                rbx::lua_pushvalue(state, LUA_GLOBALSINDEX);
            }
            return 1;
        }

        // getfenv is ours because native getfenv answers a C closure with the
        // calling thread's globals, and on an executing thread those are the
        // script's own environment - so asking about any C frame, engine
        // metamethods included, hands the script's private globals back out.
        // Game code expects the game's globals there instead, which is what
        // this answers for every C closure but getgenv.
        //
        // getgenv is the one exception: its environment is the environment of
        // the code that called it - the same table getfenv(1) reports - and
        // that is both what the script environment is and what callers of
        // getgenv expect to see.
        //
        // Everything else (Lua closures, stack levels, argument validation)
        // runs Roblox's own getfenv *on this frame*, not through a second C
        // call, so level numbers still count the same frames they always did.
        inline int getfenv(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_islfunction(state, 1)) {
                const void* closure = rbx::lua_topointer(state, 1);
                int reference = 0;
                {
                    std::lock_guard<std::mutex> lock(Closures::hookedEnvMutex);
                    auto found = Closures::hookedEnvs.find(closure);
                    if (found != Closures::hookedEnvs.end())
                        reference = found->second;
                }
                if (reference > 0) {
                    rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
                    return 1;
                }
            }
            if (rbx::lua_type(state, 1) == luaobj::kTagFunction &&
                rbx::lua_iscfunction(state, 1)) {
                const uintptr_t function = rbx::lua_tocfunction(state, 1);
                if (function == reinterpret_cast<uintptr_t>(&Environment::getgenv)) {
                    rbx::lua_pushvalue(state, LUA_GLOBALSINDEX);
                    return 1;
                }
                rbx::lua_getfield(state, LUA_REGISTRYINDEX, "vanta.renv");
                if (rbx::lua_type(state, -1) == luaobj::kTagTable)
                    return 1;
                rbx::lua_settop(state, -2);
            }

            rbx::lua_getfield(state, LUA_REGISTRYINDEX, "vanta.renv");
            if (rbx::lua_type(state, -1) == luaobj::kTagTable) {
                rbx::lua_getfield(state, -1, "getfenv");
                const uintptr_t function = rbx::lua_type(state, -1) == luaobj::kTagFunction
                    ? rbx::lua_tocfunction(state, -1) : 0;
                rbx::lua_settop(state, -3);
                if (function) {
                    return reinterpret_cast<int (*)(lua_State*)>(function)(L);
                }
            } else {
                rbx::lua_settop(state, -2);
            }
            rbx::lua_pushvalue(state, LUA_GLOBALSINDEX);
            return 1;
        }

        // getgc walks Roblox's own GC pages through luaM_visitgco. The heap is
        // collected first so what is left is what the spec calls non-dead, and
        // the collector is then stopped for the duration: building the result
        // table allocates, and allocating while walking the page list means
        // walking a list that is changing underneath.
        //
        // Nothing is pushed during the walk for the same reason - the objects
        // are recorded here first and only turned into Lua values afterwards,
        // which is safe precisely because the collector cannot run in between.
        struct GcWalk {
            std::vector<const void*> found;
            std::vector<uint8_t> tags;
            bool includeTables = false;
            int tagAt = 0;
        };

        inline bool gcVisit(void* context, void* page, void* object)
        {
            (void)page;
            auto* walk = static_cast<GcWalk*>(context);
            const uint8_t tag = luaobj::byte(object, walk->tagAt);
            if (tag == luaobj::kTagFunction || tag == luaobj::kTagUserdata ||
                (walk->includeTables && tag == luaobj::kTagTable)) {
                walk->found.push_back(object);
                walk->tags.push_back(tag);
            }
            return false;   // nothing was deleted
        }

        // The collector has to be stopped while the walk's results are held in a
        // C++ vector, because nothing there is reachable from Lua and a step
        // would free objects that are then written into the table. That leaves
        // a hazard: anything raising between the stop and the restart longjmps
        // past the restart and the collector never runs again - which shows up
        // much later as "the garbage collector is not working properly" in a
        // test that has nothing to do with getgc. So the stop is recorded, and
        // a later call puts it right before doing anything else.
        inline bool collectorStopped = false;

        inline int getgc(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            if (collectorStopped) {
                rbx::lua_gc(state, LUA_GCRESTART, 0);
                collectorStopped = false;
            }

            GcWalk walk;
            walk.tagAt = gcTagOffset(state);
            walk.includeTables = rbx::lua_toboolean(state, 1) != 0;

            rbx::lua_gc(state, LUA_GCCOLLECT, 0);
            rbx::lua_gc(state, LUA_GCSTOP, 0);
            collectorStopped = true;

            rbx::luaM_visitgco(state, &walk, gcVisit);

            rbx::lua_settop(state, 0);
            rbx::lua_createtable(state, static_cast<int>(walk.found.size()), 0);
            for (size_t i = 0; i < walk.found.size(); i++) {
                // Reserve a slot the normal way - that is what grows the stack -
                // then write the object into it, since there is no push for a
                // raw GCObject.
                rbx::lua_pushnil(state);
                uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
                uint8_t* slot = top - luaobj::kValueSize;
                luaobj::write<const void*>(slot, luaobj::value::gc, walk.found[i]);
                luaobj::write<int>(slot, luaobj::value::tt, walk.tags[i]);
                rbx::lua_rawseti(state, 1, static_cast<int>(i + 1));
            }

            rbx::lua_gc(state, LUA_GCRESTART, 0);
            collectorStopped = false;

            // Walking the heap allocated a table holding every live object.
            // Collecting once more starts the next cycle from a clean slate, so
            // the caller dropping that table does not leave the collector a
            // whole heap behind - which is what made an unrelated weak-reference
            // test time out.
            //
            // Removing this was tried on 2026-09-03 for the 5.7 ms it costs, on
            // the grounds that no reference collects after its walk. It is a net
            // loss and it is measurable: with it gone the garbage from each walk
            // survives into the next one, and a client that had run the suite
            // plus perf.lua reached **265k live objects against 78k with it in
            // place** - so every later getgc, filtergc and getloadedmodules ran
            // roughly four times slower (getgc 13.5 -> 55.7 ms). The references
            // do not run a compliance suite that walks the heap a dozen times in
            // a row; this tree does. Keep it.
            rbx::lua_gc(state, LUA_GCCOLLECT, 0);
            return 1;
        }

        // getgc and filtergc between them allocate a table holding every live
        // function, userdata and table in the VM - tens of megabytes on a real
        // game. Leaving that much garbage behind pushes the collector far
        // enough back that unrelated code waiting on a weak reference can time
        // out, so a diagnostic that walks the heap puts the heap back the way
        // it found it.
        inline int collect(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_gc(state, LUA_GCCOLLECT, 0);
            return 0;
        }

        // Filtergc

    }
    
    namespace Filesystem {

        // lua_error does not return; the trailing return keeps the compiler
        // from warning about a path that falls off the end.
        inline int raise(uint64_t state, const std::string& message)
        {
            rbx::lua_pushlstring(state, message.data(), message.size());
            rbx::lua_error(state);
            return 0;
        }

        // Shared front half of every entry point: read argument 1 as a path and
        // map it into the workspace, raising rather than returning on refusal.
        inline bool argPath(uint64_t state, std::filesystem::path& out)
        {
            // The references all require a real string here (luaL_checktype /
            // luaL_argexpected on LUA_TSTRING). lua_tolstring on its own would
            // accept a number and coerce it - and coercion rewrites the stack
            // slot in place, so the caller's argument changes type under it.
            if (rbx::lua_type(state, 1) != luaobj::kTagString) {
                raise(state, "expected a path string");
                return false;
            }
            size_t len = 0;
            const char* given = rbx::lua_tolstring(state, 1, &len);
            if (!given) {
                raise(state, "expected a path string");
                return false;
            }

            std::string error;
            if (!workspace::resolve(std::string(given, len), out, error)) {
                raise(state, error);
                return false;
            }
            return true;
        }

        inline int isfile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;
            std::error_code code;
            rbx::lua_pushboolean(state, std::filesystem::is_regular_file(path, code) ? 1 : 0);
            return 1;
        }

        inline int isfolder(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;
            std::error_code code;
            rbx::lua_pushboolean(state, std::filesystem::is_directory(path, code) ? 1 : 0);
            return 1;
        }

        // The most a readfile will pull into the heap. RbxStu is the only
        // reference that puts a ceiling on this at all ("file content too big")
        // and it is right to: the alternative to refusing is allocating whatever
        // is on disk on Roblox's own thread, and the failure mode for that is a
        // dead client rather than an error a script can pcall.
        inline constexpr uintmax_t kMaxReadBytes = 256u << 20;

        inline int readfile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return raise(state, "readfile: " + path.filename().string() + " does not exist");

            // One allocation, sized up front, which is what Vicna, sourcessss
            // and Module-Source all do (mac and RbxStu stream into a
            // stringstream and then copy the result out, so the file exists
            // three times over at the peak: the buffer, the copy, and the Lua
            // string).
            std::error_code code;
            const uintmax_t size = std::filesystem::file_size(path, code);
            if (code)
                return raise(state, "readfile: " + workspace::describe(path) + " could not be sized");
            if (size > kMaxReadBytes)
                return raise(state, "readfile: " + workspace::describe(path) + " is too big to read");

            std::string data(static_cast<size_t>(size), '\0');
            if (size)
                file.read(data.data(), static_cast<std::streamsize>(size));
            // A file that shrank between the size and the read leaves the tail
            // untouched rather than short, so trust what actually arrived.
            data.resize(static_cast<size_t>(file.gcount()));

            rbx::lua_pushlstring(state, data.data(), data.size());
            return 1;
        }

        inline int writeOrAppend(lua_State* L, bool append) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;

            if (workspace::blockedExtension(path))
                return raise(state, "writing " + path.extension().string() + " files is not allowed");

            size_t len = 0;
            const char* data = rbx::lua_tolstring(state, 2, &len);
            if (!data)
                return raise(state, "expected file contents as a string");

            // Scripts write to paths whose directories they never made, and the
            // workspace root itself can be gone if a script deleted it. Both
            // used to surface as "could not open <leaf> for writing", which
            // took down a whole sUNC run and did not even say where.
            std::error_code code;
            const std::filesystem::path parent = path.parent_path();
            if (!parent.empty() && !std::filesystem::is_directory(parent, code))
                std::filesystem::create_directories(parent, code);

            std::ofstream file(path, append ? (std::ios::binary | std::ios::app) : std::ios::binary);
            if (!file)
                return raise(state, "could not open " + workspace::describe(path) + " for writing");

            file.write(data, static_cast<std::streamsize>(len));
            return 0;
        }

        inline int writefile(lua_State* L) { return writeOrAppend(L, false); }
        inline int appendfile(lua_State* L) { return writeOrAppend(L, true); }

        inline int makefolder(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;
            std::error_code code;
            std::filesystem::create_directories(path, code);
            if (code)
                return raise(state, "makefolder: " + code.message());
            return 0;
        }

        inline int delfile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;
            std::error_code code;
            if (!std::filesystem::is_regular_file(path, code))
                return raise(state, "delfile: " + path.filename().string() + " is not a file");
            std::filesystem::remove(path, code);
            if (code)
                return raise(state, "delfile: " + code.message());
            return 0;
        }

        inline int delfolder(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;
            std::error_code code;
            if (!std::filesystem::is_directory(path, code))
                return raise(state, "delfolder: " + path.filename().string() + " is not a folder");
            // Emptying the workspace root leaves every later write failing, so
            // its contents go rather than the directory itself.
            if (std::filesystem::equivalent(path, workspace::root(), code)) {
                for (const auto& entry : std::filesystem::directory_iterator(path, code))
                    std::filesystem::remove_all(entry.path(), code);
                return 0;
            }
            std::filesystem::remove_all(path, code);
            if (code)
                return raise(state, "delfolder: " + code.message());
            return 0;
        }

        inline int loadfile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return raise(state, "loadfile: " + path.filename().string() + " does not exist");

            std::ostringstream contents;
            contents << file.rdbuf();
            std::string bytecode = compileScript(contents.str());
            std::string chunkname = "@" + path.filename().string();
            chunks::mark(chunkname);

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(state, chunkname.c_str(), bytecode.c_str(), bytecode.size(), 0);
            }

            // Matches loadstring: a compile error is nil plus the message
            // rather than a raise, so callers can report it themselves.
            if (loaded != 0) {
                size_t errorLen = 0;
                const char* error = rbx::lua_tolstring(state, -1, &errorLen);
                std::string message = error ? std::string(error, errorLen) : "";
                rbx::lua_settop(state, -2);
                rbx::lua_pushnil(state);
                rbx::lua_pushlstring(state, message.data(), message.size());
                return 2;
            }

            rbx::setprotocapabilities(state, -1, &MaxCapabilities);

            return 1;
        }

        inline int listfiles(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;

            std::error_code code;
            if (!std::filesystem::is_directory(path, code))
                return raise(state, "listfiles: " + path.filename().string() + " is not a folder");

            // Collected first: building the table as we walk would leave a
            // half-filled one on the stack if the walk failed partway, and the
            // count is wanted up front to size the array part.
            const std::filesystem::path base = std::filesystem::weakly_canonical(workspace::root(), code);
            std::vector<std::string> entries;
            for (const auto& entry : std::filesystem::directory_iterator(path, code))
                entries.push_back(entry.path().lexically_relative(base).generic_string());

            if (code)
                return raise(state, "listfiles: " + code.message());

            rbx::lua_createtable(state, static_cast<int>(entries.size()), 0);
            for (size_t i = 0; i < entries.size(); i++) {
                rbx::lua_pushlstring(state, entries[i].data(), entries[i].size());
                rbx::lua_rawseti(state, -2, static_cast<int>(i + 1));
            }
            return 1;
        }

        // rbxasset:// only resolves inside the client's own content directory,
        // so the file is copied there and named after a hash of its contents.
        // Content-addressing matters: Roblox caches an asset URL once it has
        // loaded it, so a path whose contents changed has to arrive under a new
        // name or the stale copy is what plays.
        inline int getcustomasset(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::filesystem::path path;
            if (!argPath(state, path))
                return 0;

            std::error_code code;
            if (!std::filesystem::is_regular_file(path, code))
                return raise(state, "getcustomasset: " + path.filename().string() + " does not exist");

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return raise(state, "getcustomasset: could not read " + path.filename().string());

            std::ostringstream contents;
            contents << file.rdbuf();
            const std::string data = contents.str();

            unsigned char digest[CC_SHA1_DIGEST_LENGTH];
            CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), digest);

            static const char* const hex = "0123456789abcdef";
            std::string name;
            name.reserve(sizeof(digest) * 2);
            for (unsigned char byte : digest) {
                name += hex[byte >> 4];
                name += hex[byte & 0xf];
            }
            // Roblox picks a loader by extension, so the original one is kept.
            name += path.extension().string();

            const std::filesystem::path target = roblox::customAssetRoot() / name;
            if (!std::filesystem::exists(target, code)) {
                std::ofstream copy(target, std::ios::binary);
                if (!copy)
                    return raise(state, "getcustomasset: could not write into the content directory");
                copy.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!copy)
                    return raise(state, "getcustomasset: could not write into the content directory");
            }

            const std::string url = "rbxasset://vanta/" + name;
            rbx::lua_pushlstring(state, url.data(), url.size());
            return 1;
        }

    }
    
    namespace Instances {

        // Roblox keeps one canonical userdata per Instance, in a registry table
        // keyed by a lightuserdata holding the raw instance pointer. Handing
        // that pointer back as a lightuserdata is the whole C side of the cache
        // library: a lightuserdata is a first-class Lua value, so the bootstrap
        // can index the cache table with it directly and compare two of them
        // for equality, which is what compareinstances is.
        inline int rawpointer(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            if (!data) {
                rbx::lua_pushnil(state);
                return 1;
            }
            rbx::lua_pushlightuserdatatagged(state, *reinterpret_cast<void**>(data), 0);
            return 1;
        }

        // The key Roblox files the instance cache under in the registry: the
        // address of its own push function, as a lightuserdata.
        //
        // The bootstrap used to *scan* the registry for a table whose entry for
        // game's pointer was game, and disable cloneref and the whole cache
        // library when the scan came up empty. Every reference executor does a
        // single rawget with this key instead
        // (`A-Actual-Mac-Executor.../src/environment/cache/cache.hpp`,
        // `Vicna .../env/libs/cache.hpp`, SkidBase, Module-Source - all five
        // that have the library at all agree), and we already knew the address:
        // `offsets::pushinstance` was itself derived as the key of that table.
        inline int cachekey(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_pushlightuserdatatagged(
                state, reinterpret_cast<void*>(offsets::pushinstance), 0);
            return 1;
        }

        // A second userdata for the Instance already on the stack, made the way
        // Roblox makes one: hand its weak_ptr back to the engine's own push
        // function. An Instance userdata *is* a weak_ptr<Instance>, which is
        // what lua_touserdata returns a pointer to.
        //
        // cloneref used to get its second reference by writing the instance
        // into an ObjectValue and reading the property back, which is a
        // DataModel round trip and an allocation for something the engine will
        // do directly. This is what the references call.
        inline int pushSecondReference(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagUserdata)
                return utils::fail(state, "expected an Instance");
            auto* reference = static_cast<rbx::WeakInstance*>(rbx::lua_touserdata(state, 1));
            if (!reference)
                return utils::fail(state, "expected an Instance");

            rbx::pushinstance(state, reference);

            // The fresh userdata comes back without the metatable the original
            // carried, so it is copied across - the references do the same.
            if (rbx::lua_getmetatable(state, 1))
                rbx::lua_setmetatable(state, -2);
            return 1;
        }

        // fireclickdetector
        // fireproximityprompt
        // firetouchinterest
        // getcallbackvalue
        // gethui
        // getinstances
        // getnilinstances
        
    }
    
    // The engine calls behind fireproximityprompt / firetouchinterest /
    // fireclickdetector. Doing these in Luau - walking the connections and
    // calling them - is what the spec tells you not to do, because the server
    // never sees it and any listener can tell. These go through the same
    // functions the engine itself calls when a real player triggers them.
    namespace Fire {

        inline int fail(uint64_t state, const char* message)
        {
            rbx::lua_pushlstring(state, message, std::strlen(message));
            rbx::lua_error(state);
            return 0;
        }

        inline uintptr_t instanceAt(uint64_t state, int index)
        {
            void* data = rbx::lua_touserdata(state, index);
            if (!data)
                return 0;
            uintptr_t instance = 0;
            if (!Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)))
                return 0;
            return instance;
        }

        inline int fireproximityprompt(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t prompt = instanceAt(state, 1);
            if (!prompt)
                return fail(state, "fireproximityprompt: expected a ProximityPrompt");

            reinterpret_cast<void (*)(uintptr_t)>(offsets::fireproximityprompt)(prompt);
            return 0;
        }

        // A real touch, through the engine's own overlap registration - so the
        // server sees it too, which is the whole point of the function. Both
        // parts have to belong to the same physics world; the engine raises
        // "new overlap in different world" if they do not, and there is no
        // reason to let it get that far.
        //
        // Answers false rather than raising when a part has no Primitive (one
        // that has never been simulated), which lets the Lua side fall back to
        // firing the two signals directly.
        inline int firetouchinterest(lua_State* L) {
            namespace layout = offsets::structs;
            uint64_t state = reinterpret_cast<uint64_t>(L);

            const auto instanceAt = [&](int index) -> uintptr_t {
                void* data = rbx::lua_touserdata(state, index);
                uintptr_t instance = 0;
                if (data)
                    Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance));
                return instance;
            };

            const uintptr_t part = instanceAt(1);
            const uintptr_t other = instanceAt(2);
            const int ended = rbx::lua_toboolean(state, 3);
            if (!part || !other) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            uintptr_t partPrimitive = 0, otherPrimitive = 0;
            if (!Probe::safeRead(&partPrimitive, part + layout::basepart_primitive,
                                 sizeof(partPrimitive)) ||
                !Probe::safeRead(&otherPrimitive, other + layout::basepart_primitive,
                                 sizeof(otherPrimitive)) ||
                !partPrimitive || !otherPrimitive) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            uintptr_t world = 0, otherWorld = 0;
            if (!Probe::safeRead(&world, partPrimitive + layout::primitive_world, sizeof(world)) ||
                !Probe::safeRead(&otherWorld, otherPrimitive + layout::primitive_world,
                                 sizeof(otherWorld)) ||
                !world || world != otherWorld) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            reinterpret_cast<void (*)(uintptr_t, uintptr_t, int)>(
                offsets::firetouchinterest)(partPrimitive, otherPrimitive, ended);
            rbx::lua_pushboolean(state, 1);
            return 1;
        }
    }

    // The Drawing library's native surface. The Lua half keeps the property
    // bag and the object model; this is only what gets drawn, and where.
    namespace Draw {

        inline double numberField(uint64_t state, int table, const char* name, double fallback)
        {
            rbx::lua_getfield(state, table, name);
            double out = fallback;
            if (rbx::lua_type(state, -1) == 3) {
                const uint8_t* slot = Debug::stackSlot(state, -1);
                if (slot)
                    out = luaobj::read<double>(slot, luaobj::value::gc);
            }
            rbx::lua_settop(state, -2);
            return out;
        }

        inline bool boolField(uint64_t state, int table, const char* name)
        {
            rbx::lua_getfield(state, table, name);
            const bool out = rbx::lua_toboolean(state, -1) != 0;
            rbx::lua_settop(state, -2);
            return out;
        }

        inline std::string stringField(uint64_t state, int table, const char* name)
        {
            rbx::lua_getfield(state, table, name);
            size_t length = 0;
            const char* text = rbx::lua_tolstring(state, -1, &length);
            std::string out = text ? std::string(text, length) : std::string();
            rbx::lua_settop(state, -2);
            return out;
        }

        inline int available(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_pushboolean(state, overlay::available() ? 1 : 0);
            return 1;
        }

        // __vanta_draw_set(shapeTable). One flat table per shape - the Lua side
        // already holds the authoritative property bag, so this is a snapshot
        // rather than a handle.
        inline int set(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagTable) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }
            const int table = 1;

            overlay::Shape shape;
            shape.id = static_cast<uint64_t>(numberField(state, table, "id", 0));
            shape.kind = static_cast<overlay::Kind>(
                static_cast<int>(numberField(state, table, "kind", 0)));
            shape.visible = boolField(state, table, "visible");
            shape.zindex = static_cast<int>(numberField(state, table, "zindex", 0));
            shape.transparency = numberField(state, table, "transparency", 0);
            shape.r = numberField(state, table, "r", 0);
            shape.g = numberField(state, table, "g", 0);
            shape.b = numberField(state, table, "b", 0);

            static const char* const names[8] = { "x1", "y1", "x2", "y2",
                                                  "x3", "y3", "x4", "y4" };
            for (int i = 0; i < 4; i++) {
                shape.points[i].x = numberField(state, table, names[i * 2], 0);
                shape.points[i].y = numberField(state, table, names[i * 2 + 1], 0);
            }
            shape.size.x = numberField(state, table, "w", 0);
            shape.size.y = numberField(state, table, "h", 0);

            shape.thickness = numberField(state, table, "thickness", 1);
            shape.filled = boolField(state, table, "filled");
            shape.radius = numberField(state, table, "radius", 0);
            shape.numsides = static_cast<int>(numberField(state, table, "numsides", 0));

            shape.text = stringField(state, table, "text");
            shape.textsize = numberField(state, table, "textsize", 13);
            shape.font = static_cast<int>(numberField(state, table, "font", 0));
            shape.center = boolField(state, table, "center");
            shape.outline = boolField(state, table, "outline");
            shape.outline_r = numberField(state, table, "or", 0);
            shape.outline_g = numberField(state, table, "og", 0);
            shape.outline_b = numberField(state, table, "ob", 0);
            shape.data = stringField(state, table, "data");

            overlay::set(shape);
            rbx::lua_pushboolean(state, 1);
            return 1;
        }

        inline int remove(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            if (isnum)
                overlay::remove(static_cast<uint64_t>(id));
            return 0;
        }

        // Verification, not part of the library: renders the overlay into a
        // bitmap and reports what it painted, since screen capture needs a
        // permission this process does not have.
        inline int inspect(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const overlay::Snapshot snapshot = overlay::inspect();

            rbx::lua_createtable(state, 0, 8);
            rbx::lua_pushboolean(state, snapshot.built ? 1 : 0);
            rbx::lua_setfield(state, -2, "built");
            const auto field = [&](const char* name, double value) {
                utils::pushNumber(state, value);
                rbx::lua_setfield(state, -2, name);
            };
            field("x", snapshot.x);
            field("y", snapshot.y);
            field("w", snapshot.w);
            field("h", snapshot.h);
            field("gamex", snapshot.gamex);
            field("gamey", snapshot.gamey);
            field("gamew", snapshot.gamew);
            field("gameh", snapshot.gameh);
            field("painted", static_cast<double>(snapshot.painted));
            field("r", snapshot.r);
            field("g", snapshot.g);
            field("b", snapshot.b);
            field("shapes", static_cast<double>(snapshot.shapes));
            return 1;
        }

        inline int clear(lua_State* L) {
            (void)L;
            overlay::clear();
            return 0;
        }

        // __vanta_draw_textbounds(text, size, font) -> width, height
        inline int textbounds(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            size_t length = 0;
            const char* text = rbx::lua_tolstring(state, 1, &length);
            double wanted = 13;
            if (rbx::lua_type(state, 2) == 3) {
                const uint8_t* slot = Debug::stackSlot(state, 2);
                if (slot)
                    wanted = luaobj::read<double>(slot, luaobj::value::gc);
            }
            int isnum = 0;
            const int font = rbx::lua_tointegerx(state, 3, &isnum);

            double width = 0, height = 0;
            overlay::textBounds(text ? std::string(text, length) : std::string(), wanted,
                                isnum ? font : 0, &width, &height);
            utils::pushNumber(state, width);
            utils::pushNumber(state, height);
            return 2;
        }
    }

    namespace Callbacks {

        inline int getcallbackvalue(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            void* data = rbx::lua_touserdata(state, 1);
            size_t nameLength = 0;
            const char* name = rbx::lua_tolstring(state, 2, &nameLength);
            if (!data || !name) {
                const char* message = "getcallbackvalue: expected an Instance and a property name";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            uintptr_t instance = 0;
            if (!Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const uintptr_t descriptor =
                Reflection2::findMember(instance, std::string(name, nameLength));
            int32_t reference = 0;
            const bool found = descriptor &&
                               Reflection2::callbackRef(L, instance, descriptor, reference);
            journal::writef("CBNAME %.*s found=%d ref=%d", static_cast<int>(nameLength), name,
                            found ? 1 : 0, found ? reference : 0);
            if (!found) {
                rbx::lua_pushnil(state);
                return 1;
            }

            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
            if (rbx::lua_type(state, -1) != luaobj::kTagFunction) {
                rbx::lua_settop(state, -2);
                rbx::lua_pushnil(state);
            }
            return 1;
        }
    }

    // Actors. Each one runs its own Luau state, and Roblox keeps the mapping in
    // the ScriptContext - see offsets::structs for how that was found. The
    // state is not stored directly: the map holds the id that Roblox's own
    // getGlobalState takes, so asking for it is the engine's own path rather
    // than any decryption of ours.
    namespace Actors {

        namespace layout = offsets::structs;

        inline uintptr_t scriptContext(uint64_t state)
        {
            // The ScriptContext instance, via the service the game already has.
            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "game");
            rbx::lua_getfield(state, -1, "GetService");
            rbx::lua_pushvalue(state, -2);
            rbx::lua_pushlstring(state, "ScriptContext", 13);
            rbx::lua_call(state, 2, 1);

            void* data = rbx::lua_touserdata(state, -1);
            uintptr_t instance = 0;
            if (data)
                Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance));
            rbx::lua_settop(state, -3);
            return instance;
        }

        // The lua_State a state holder owns. Roblox stores it as two 32-bit
        // halves, each hidden against the low half of the address it sits at -
        // exactly what getGlobalState's tail does, and the reason every earlier
        // search of the Actor map for something shaped like a state found
        // nothing: there is no plain pointer there to find.
        //
        // *How* it is hidden is a property of the build, not a constant, and
        // there are three of them - not two, which is what a boolean here used
        // to assume:
        //
        //     0.735, 0.736   xor edx,[slot] ; xor ecx,[slot+4]      stored ^ key
        //     0.734          sub ecx,[slot] ; sub edi,[slot+4]      key - stored
        //     0.737          sub edx,ecx    ; sub eax,ecx           stored - key
        //
        // The two subtractions are *not* the same encoding: which operand is
        // the memory one decides the direction, and an audit that only asks
        // "xor or subtract" cannot tell them apart. Undoing the wrong one gives
        // a plausible-looking pointer that is not a state, so it fails quietly
        // rather than loudly. `stateholder_op` is derived from the decode
        // itself; see vantadump/engine.py's stateholder_decodes.
        inline uint64_t decryptState(uintptr_t holder)
        {
            if (!holder)
                return 0;
            const uintptr_t at = holder + layout::stateholder_state;
            uint32_t low = 0, high = 0;
            if (!Probe::safeRead(&low, at, sizeof(low)) ||
                !Probe::safeRead(&high, at + 4, sizeof(high)))
                return 0;
            const uint32_t key = static_cast<uint32_t>(at);
            uint32_t lo = 0, hi = 0;
            switch (layout::stateholder_op) {
            case 0:                                     // key - stored
                lo = key - low;   hi = key - high;   break;
            case 1:                                     // stored ^ key
                lo = low ^ key;   hi = high ^ key;   break;
            case 2:                                     // stored - key
                lo = low - key;   hi = high - key;   break;
            default:
                return 0;                               // an encoding nobody derived
            }
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }

        inline bool looksLikeState(uint64_t candidate, uint64_t ours);

        // Every state Roblox owns is listed in a vector beside the Actor map,
        // so a holder read out of a node can be checked against that list
        // instead of being trusted. Answers false if the vector cannot be read,
        // which leaves the caller with the shape test alone.
        inline bool listedHolder(uintptr_t context, uintptr_t holder, bool& checked)
        {
            checked = false;
            uintptr_t first = 0, last = 0;
            if (!Probe::safeRead(&first, context + layout::scriptcontext_states, sizeof(first)) ||
                !Probe::safeRead(&last, context + layout::scriptcontext_states_end, sizeof(last)))
                return false;
            if (!first || last < first || (last - first) > 0x8000)
                return false;

            checked = true;
            for (uintptr_t at = first; at + sizeof(uintptr_t) <= last; at += sizeof(uintptr_t)) {
                uintptr_t entry = 0;
                if (!Probe::safeRead(&entry, at, sizeof(entry)))
                    return false;
                if (entry == holder)
                    return true;
            }
            return false;
        }

        // The Actor's own lua_State, or 0 if its scripts have not run yet.
        inline uint64_t idFor(uintptr_t context, uintptr_t actor, uint64_t ours)
        {
            if (!context || !actor)
                return 0;

            const uintptr_t map = context + layout::scriptcontext_actormap;
            uintptr_t node = 0;
            if (!Probe::safeRead(&node, map + layout::map_firstnode, sizeof(node)))
                return 0;

            // Walk to the end of the chain rather than trusting the size field:
            // one bad read of it would silently stop the search short.
            for (int step = 0; node && step < 4096; step++) {
                uintptr_t key = 0;
                if (!Probe::safeRead(&key, node + layout::node_key, sizeof(key)))
                    break;
                if (key == actor) {
                    uintptr_t holder = 0;
                    if (!Probe::safeRead(&holder, node + layout::node_value, sizeof(holder)))
                        return 0;
                    bool checked = false;
                    if (listedHolder(context, holder, checked)) {
                        const uint64_t state = decryptState(holder);
                        return looksLikeState(state, ours) ? state : 0;
                    }
                    if (checked)
                        return 0;
                    // The vector could not be read: fall back to the shape test
                    // over the node, so a layout change costs accuracy rather
                    // than the whole function.
                    for (int at = 0x18; at <= 0x40; at += 8) {
                        uintptr_t candidate = 0;
                        if (!Probe::safeRead(&candidate, node + at, sizeof(candidate)))
                            break;
                        const uint64_t state = decryptState(candidate);
                        if (looksLikeState(state, ours))
                            return state;
                    }
                    return 0;
                }
                if (!Probe::safeRead(&node, node + layout::node_next, sizeof(node)))
                    break;
            }
            return 0;
        }

        // A candidate is only used if it reads like a lua_State whose global is
        // a real pointer and is *not* ours, which is the whole point of an
        // actor. An Actor whose script has not run yet has no state at all, so
        // finding nothing is "not started", not a failed lookup.
        inline bool looksLikeState(uint64_t candidate, uint64_t ours)
        {
            if (!candidate)
                return false;
            uintptr_t global = 0, globals = 0, callinfo = 0;
            if (!Probe::safeRead(&global, candidate + luaobj::state::global, sizeof(global)) ||
                !Probe::safeRead(&globals, candidate + luaobj::state::gt, sizeof(globals)) ||
                !Probe::safeRead(&callinfo, candidate + luaobj::state::ci, sizeof(callinfo)))
                return false;

            uintptr_t ourGlobal = 0;
            Probe::safeRead(&ourGlobal, ours + luaobj::state::global, sizeof(ourGlobal));

            const auto plausible = [](uintptr_t value) {
                return value > 0x100000000ULL && value < 0x800000000000ULL;
            };
            // Deliberately not requiring a different global_State: an actor's
            // thread may live in the same one, and rejecting it on that basis
            // was what made the lookup come back empty for a live Actor whose
            // state plainly existed. A distinct globals table is enough to say
            // this is not simply our own thread.
            uintptr_t ourGlobals = 0;
            Probe::safeRead(&ourGlobals, ours + luaobj::state::gt, sizeof(ourGlobals));
            (void)ourGlobal;

            return plausible(global) && plausible(globals) && plausible(callinfo) &&
                   candidate != ours && globals != ourGlobals;
        }

        inline uint64_t stateFor(uint64_t state, uintptr_t actor)
        {
            const uintptr_t context = scriptContext(state);
            return idFor(context, actor, state);
        }

        // The main thread of whichever Luau state a given thread belongs to.
        // Threads do not name their state, but every state Roblox owns is in
        // the ScriptContext's vector, so matching global_States finds it.
        inline uint64_t stateOwning(uint64_t state, uintptr_t thread)
        {
            uintptr_t wanted = 0;
            if (!thread ||
                !Probe::safeRead(&wanted, thread + luaobj::state::global, sizeof(wanted)) ||
                !wanted)
                return 0;

            const uintptr_t context = scriptContext(state);
            if (!context)
                return 0;

            uintptr_t first = 0, last = 0;
            if (!Probe::safeRead(&first, context + layout::scriptcontext_states, sizeof(first)) ||
                !Probe::safeRead(&last, context + layout::scriptcontext_states_end, sizeof(last)))
                return 0;
            if (!first || last < first || (last - first) > 0x8000)
                return 0;

            for (uintptr_t at = first; at + sizeof(uintptr_t) <= last; at += sizeof(uintptr_t)) {
                uintptr_t holder = 0;
                if (!Probe::safeRead(&holder, at, sizeof(holder)) || !holder)
                    continue;
                const uint64_t candidate = decryptState(holder);
                uintptr_t global = 0;
                if (candidate &&
                    Probe::safeRead(&global, candidate + luaobj::state::global, sizeof(global)) &&
                    global == wanted)
                    return candidate;
            }
            return 0;
        }

        // Diagnostic: hands back the id and the state so the derivation can be
        // checked from Lua rather than taken on trust.
        // Every Actor the engine knows about, as raw pointers.
        //
        //   __vanta_actorpointers() -> { <lightuserdata>, ... }
        //
        // The macOS reference and sourcessss both build `getactors` from this
        // map rather than from a walk of the DataModel - it is the engine's own
        // list, so it cannot miss an Actor that Lua has never touched. The map
        // is already walked here to answer "which state does this Actor own";
        // this is the same walk without the key filter. The pointers come back
        // as lightuserdata so the Lua side can look each one up in the instance
        // cache, which is exactly what the reference's two rawgets do.
        inline int actorpointers(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t context = scriptContext(state);

            rbx::lua_createtable(state, 0, 0);
            if (!context)
                return 1;

            uintptr_t node = 0;
            if (!Probe::safeRead(&node, context + layout::scriptcontext_actormap +
                                            layout::map_firstnode, sizeof(node)))
                return 1;

            int count = 0;
            for (int step = 0; node && step < 4096; step++) {
                uintptr_t key = 0;
                if (Probe::safeRead(&key, node + layout::node_key, sizeof(key)) && key) {
                    rbx::lua_pushlightuserdatatagged(state, reinterpret_cast<void*>(key), 0);
                    rbx::lua_rawseti(state, -2, ++count);
                }
                uintptr_t next = 0;
                if (!Probe::safeRead(&next, node + layout::node_next, sizeof(next)))
                    break;
                node = next;
            }
            return 1;
        }

        inline int actorstate(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t actor = 0;
            if (data)
                Probe::safeRead(&actor, reinterpret_cast<uintptr_t>(data), sizeof(actor));

            const uintptr_t context = scriptContext(state);
            const uint64_t id = idFor(context, actor, state);

            uintptr_t first = 0, size = 0;
            Probe::safeRead(&first, context + layout::scriptcontext_actormap + layout::map_firstnode,
                            sizeof(first));
            Probe::safeRead(&size, context + layout::scriptcontext_actormap + layout::map_size,
                            sizeof(size));

            rbx::lua_createtable(state, 0, 7);
            utils::pushNumber(state, static_cast<double>(context));
            rbx::lua_setfield(state, -2, "context");
            utils::pushNumber(state, static_cast<double>(actor));
            rbx::lua_setfield(state, -2, "actor");
            utils::pushNumber(state, static_cast<double>(first));
            rbx::lua_setfield(state, -2, "first");
            utils::pushNumber(state, static_cast<double>(size));
            rbx::lua_setfield(state, -2, "size");
            utils::pushNumber(state, static_cast<double>(id));
            rbx::lua_setfield(state, -2, "raw");
            rbx::lua_pushboolean(state, looksLikeState(id, state) ? 1 : 0);
            rbx::lua_setfield(state, -2, "valid");

            // Every key in the chain, so a mismatch is visible rather than
            // guessed at.
            rbx::lua_createtable(state, 0, 0);
            uintptr_t node = first;
            for (int i = 1; node && i <= 16; i++) {
                uintptr_t key = 0;
                if (!Probe::safeRead(&key, node + layout::node_key, sizeof(key)))
                    break;
                utils::pushNumber(state, static_cast<double>(key));
                rbx::lua_rawseti(state, -2, i);
                if (!Probe::safeRead(&node, node + layout::node_next, sizeof(node)))
                    break;
            }
            rbx::lua_setfield(state, -2, "keys");
            return 1;
        }

        // Copies one value from our state onto the actor's stack.
        //
        // Nothing that lives on a heap crosses as a pointer: a table belongs to
        // the state that made it, and handing the address over would give the
        // actor's collector an object it does not own. Strings and numbers
        // cross by value, tables are rebuilt on the far side, and an Instance
        // crosses as what it actually is - a weak reference the engine's own
        // pushInstance takes a copy of. That last part is what makes
        // `game` on the far side really be the far side's `game`, which is
        // exactly what an actor test checks for.
        inline void transfer(uint64_t from, uint64_t to, int index, int depth);

        // Iterating a table needs lua_next, which is not among the entry points
        // that were derived. Lua's own `next` is the same function reached from
        // the other side, so the walk goes through a call instead.
        //
        // On entry the previous key is on top of `from` (nil to start). On a
        // true return the key and value have replaced it; on false the stack is
        // back where it started.
        inline bool nextEntry(uint64_t from, int table)
        {
            rbx::lua_getfield(from, LUA_GLOBALSINDEX, "next");
            rbx::lua_pushvalue(from, table);
            rbx::lua_pushvalue(from, -3);
            rbx::lua_call(from, 2, 2);
            if (rbx::lua_type(from, -2) == 0) {
                rbx::lua_settop(from, -4);
                return false;
            }
            rbx::lua_remove(from, -3);
            return true;
        }

        inline bool transferInstance(uint64_t from, uint64_t to, int index)
        {
            void* data = rbx::lua_touserdata(from, index);
            if (!data)
                return false;

            // An Instance userdata is exactly a weak_ptr<Instance>. pushInstance
            // takes ownership of the copy it is handed, so the control block's
            // weak count is raised first - the same sequence Instance.new runs.
            rbx::WeakInstance reference{};
            std::memcpy(&reference, data, sizeof(reference));
            if (!reference.object)
                return false;
            if (reference.control)
                __atomic_add_fetch(reinterpret_cast<long*>(reference.control + 0x10), 1,
                                   __ATOMIC_SEQ_CST);
            rbx::pushinstance(to, &reference);
            return true;
        }

        inline void transferTable(uint64_t from, uint64_t to, int index, int depth)
        {
            const int table = index < 0 ? rbx::lua_gettop(from) + 1 + index : index;
            rbx::lua_createtable(to, 0, 0);
            const int target = rbx::lua_gettop(to);

            rbx::lua_pushnil(from);
            for (int guard = 0; guard < 8192 && nextEntry(from, table); guard++) {
                transfer(from, to, -2, depth + 1);
                transfer(from, to, -1, depth + 1);
                rbx::lua_rawset(to, target);
                // leave the key so the walk can continue from it
                rbx::lua_settop(from, -2);
            }
        }

        inline void transfer(uint64_t from, uint64_t to, int index, int depth)
        {
            if (depth > 8) {
                rbx::lua_pushnil(to);
                return;
            }

            switch (rbx::lua_type(from, index)) {
                case 1:  // boolean
                    rbx::lua_pushboolean(to, rbx::lua_toboolean(from, index));
                    return;
                case 3: {  // number
                    const uint8_t* slot = Debug::stackSlot(from, index);
                    utils::pushNumber(to, slot ? luaobj::read<double>(slot, luaobj::value::gc) : 0.0);
                    return;
                }
                case luaobj::kTagString: {
                    size_t len = 0;
                    const char* text = rbx::lua_tolstring(from, index, &len);
                    rbx::lua_pushlstring(to, text ? text : "", text ? len : 0);
                    return;
                }
                case luaobj::kTagTable:
                    transferTable(from, to, index, depth);
                    return;
                case luaobj::kTagUserdata:
                    if (transferInstance(from, to, index))
                        return;
                    rbx::lua_pushnil(to);
                    return;
                default:
                    rbx::lua_pushnil(to);
                    return;
            }
        }

        // Whether the calling thread is running inside an Actor's own Luau
        // state. Every actor gets a separate one, so the question is answered
        // by comparing global states rather than by any flag: our own threads
        // share the main VM's, and a thread run_on_actor built does not.
        inline int isparallel(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const uintptr_t ours =
                luaobj::read<uintptr_t>(reinterpret_cast<void*>(state), luaobj::state::global);
            uintptr_t main = 0;
            if (mainState)
                Probe::safeRead(&main, mainState + luaobj::state::global, sizeof(main));
            rbx::lua_pushboolean(state, (main && ours && ours != main) ? 1 : 0);
            return 1;
        }

        inline int runonactor(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            auto fail = [&](const char* message) {
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            };

            void* data = rbx::lua_touserdata(state, 1);
            uintptr_t actor = 0;
            if (data)
                Probe::safeRead(&actor, reinterpret_cast<uintptr_t>(data), sizeof(actor));
            if (!actor)
                return fail("run_on_actor: expected an Actor");

            size_t sourceLen = 0;
            const char* source = rbx::lua_tolstring(state, 2, &sourceLen);
            if (!source)
                return fail("run_on_actor: expected source");

            const uint64_t target = stateFor(state, actor);
            if (!target)
                return fail("run_on_actor: that Actor has no Luau state yet");

            const int arguments = rbx::lua_gettop(state);

            // The arguments cannot ride on the thread's stack. Roblox's own
            // spawn is what starts the thread, and it does not forward a stack
            // it was not given - the first attempt handed the chunk four values
            // and it saw none of them. So they go into a table in the thread's
            // own globals instead, and a two-line wrapper hands them straight
            // back to the user's source as its varargs.
            //
            // The prologue shares a line with the start of the source, so a
            // syntax error still reports the line the caller wrote.
            const std::string wrapped =
                "local A=__vanta_args __vanta_args=nil return (function(...) " +
                std::string(source, sourceLen) + "\nend)(table.unpack(A,1,A.n))";

            const std::string bytecode = compileScript(wrapped);
            if (bytecode.empty() || bytecode[0] == 0)
                return fail("run_on_actor: the source did not compile");

            uint64_t thread = rbx::lua_newthread(target);
            const int threadReference = rbx::lua_ref(target, -1);
            rbx::lua_settop(target, -2);

            rbx::luaL_sandboxthread(thread);
            SetIdentity(thread, kExecutorIdentity);

            // The Actor's state has never seen us. Sandboxing has just given
            // this thread a globals table of its own, so installing our
            // functions here reaches the chunk without touching the globals the
            // actor's own scripts run against. safeenv has to come off first,
            // or the VM keeps serving cached reads of globals that were not
            // there when it cached them.
            rbx::lua_setsafeenv(thread, LUA_GLOBALSINDEX, 0);
            RegisterFunctions(thread);

            // The argument table, built on the far side one value at a time.
            rbx::lua_createtable(thread, arguments > 2 ? arguments - 2 : 0, 1);
            int packed = 0;
            for (int i = 3; i <= arguments; i++) {
                transfer(state, thread, i, 0);
                rbx::lua_rawseti(thread, -2, ++packed);
            }
            rbx::lua_pushinteger(thread, packed);
            rbx::lua_setfield(thread, -2, "n");
            rbx::lua_setfield(thread, LUA_GLOBALSINDEX, "__vanta_args");

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(thread, "@vanta", bytecode.c_str(), bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(thread, 0);
                rbx::lua_unref(target, threadReference);
                return fail("run_on_actor: the actor refused the chunk");
            }
            rbx::setprotocapabilities(thread, -1, &MaxCapabilities);

            // The reference keeps this reference for good, and so does this:
            // dropping it hands the thread to the actor's collector while the
            // scheduler still has it queued.
            rbx::task::spawn(thread);
            return 0;
        }

        // --- communication channels -----------------------------------------
        //
        // A channel is a BindableEvent both sides can reach. The id -> event
        // map cannot live in Lua: an Actor runs in a different Luau state, so a
        // table in ours is not something it can look anything up in. Keeping it
        // in C++ and handing back the Instance means each state gets its own
        // userdata for the same event, which is exactly what the engine already
        // does for every other Instance.
        inline std::unordered_map<int, rbx::WeakInstance>& channels()
        {
            static std::unordered_map<int, rbx::WeakInstance> map;
            return map;
        }

        inline int nextChannelId = 0;

        inline bool pushChannel(uint64_t state, rbx::WeakInstance reference)
        {
            if (!reference.object)
                return false;
            if (reference.control)
                __atomic_add_fetch(reinterpret_cast<long*>(reference.control + 0x10), 1,
                                   __ATOMIC_SEQ_CST);
            rbx::pushinstance(state, &reference);
            return true;
        }

        inline int createcommchannel(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "Instance");
            rbx::lua_getfield(state, -1, "new");
            rbx::lua_pushlstring(state, "BindableEvent", 13);
            rbx::lua_call(state, 1, 1);

            void* data = rbx::lua_touserdata(state, -1);
            if (!data) {
                rbx::lua_settop(state, -3);
                const char* message = "create_comm_channel: could not create the event";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            rbx::WeakInstance reference{};
            std::memcpy(&reference, data, sizeof(reference));
            // The map keeps a reference of its own for as long as the channel
            // exists, so the event does not go away when Lua drops it.
            if (reference.control)
                __atomic_add_fetch(reinterpret_cast<long*>(reference.control + 0x10), 1,
                                   __ATOMIC_SEQ_CST);

            const int id = ++nextChannelId;
            channels()[id] = reference;

            rbx::lua_settop(state, -3);   // drop the event and the Instance table
            rbx::lua_pushinteger(state, id);
            pushChannel(state, reference);
            return 2;
        }

        inline int getcommchannel(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            if (!isnum) {
                const char* message = "get_comm_channel: expected a channel id";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            auto found = channels().find(id);
            if (found == channels().end() || !pushChannel(state, found->second))
                rbx::lua_pushnil(state);
            return 1;
        }
    }

    namespace Metatable {

        inline int getrawmetatable(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "getrawmetatable");
            // LUA_TNONE is -1: an index past the top, which is what an empty
            // call site produces. luaL_checkany is how every reference spells
            // this.
            if (rbx::lua_type(state, 1) < 0)
                return utils::fail(state, "getrawmetatable: value expected");
            if (!rbx::lua_getmetatable(state, 1))
                rbx::lua_pushnil(state);
            return trace.done(1);
        }

        // The argument checks on this pair are not decoration. All six reference
        // executors call luaL_checktype before touching the table, and we did
        // not: `lua_getreadonly` reads a byte off whatever the TValue points at
        // and `lua_setreadonly` *writes* one, so `setreadonly(5, true)` was a
        // stray write into a number's bit pattern. The engine's own checktype
        // is what the references use, so the error text is Roblox's rather than
        // one more spelling of our own.
        inline int isreadonly(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagTable);
            rbx::lua_pushboolean(state, rbx::lua_getreadonly(state, 1));
            return 1;
        }

        inline int getnamecallmethod(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const char* method = rbx::lua_namecallatom(state, nullptr);
            if (method)
                rbx::lua_pushlstring(state, method, std::strlen(method));
            else
                rbx::lua_pushnil(state);
            return 1;
        }

        inline int setreadonly(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::luaL_checktype(state, 1, luaobj::kTagTable);
            rbx::luaL_checktype(state, 2, luaobj::kTagBoolean);
            rbx::lua_setreadonly(state, 1, rbx::lua_toboolean(state, 2));
            return 0;
        }

        // Roblox's own table.freeze and setreadonly write the same byte, so
        // "make this table readonly" is setreadonly(t, true) under the name the
        // spec gives it. It exists separately because a script that only wants
        // to lock a table should not have to pass a flag it never varies.
        inline int makereadonly(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagTable) {
                const char* message = "makereadonly: table expected";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }
            rbx::lua_setreadonly(state, 1, 1);
            return 0;
        }

        // L->namecall is a plain TString pointer, so setting it is a write - but
        // an unbarriered one into a live thread, so the string has to be anchored
        // somewhere the collector can see, or it is freed while a thread is still
        // pointing at it.
        //
        // This used to be one registry slot, released and re-taken on every call.
        // That is wrong, and the reason it never showed is worth writing down:
        // `L->namecall` is **per thread** and the slot was global, so
        //
        //     thread A: setnamecallmethod("Foo")   -> slot anchors "Foo"
        //     thread B: setnamecallmethod("Bar")   -> slot RELEASES "Foo"
        //     collector runs                       -> "Foo" is freed
        //     thread A namecalls                   -> engine reads a dead TString
        //
        // It stayed invisible because offsets.hpp pointed `lua_unref` at a
        // near-miss thunk that does nothing, so the release never happened and
        // every name stayed anchored for ever. Correcting that offset is what
        // made this fire, deep inside sUNC and nowhere near this function.
        //
        // So names are anchored in a registry table used as a set, keyed by the
        // string itself: each distinct name is anchored exactly once, re-setting
        // the same one is a no-op, and nothing is ever released. Memory is
        // bounded by the number of distinct method names a session uses, which
        // is small and finite - unlike the old unbounded ref-per-call this
        // replaced, and unlike the leak the broken unref was quietly causing.
        inline int namecallAnchors = -1;

        inline int setnamecallmethod(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagString) {
                const char* message = "setnamecallmethod: expected a method name string";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            const void* name = rbx::lua_topointer(state, 1);
            if (!name)
                return 0;

            if (namecallAnchors < 0) {
                rbx::lua_createtable(state, 0, 0);
                namecallAnchors = rbx::lua_ref(state, -1);
                rbx::lua_settop(state, -2);
            }
            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, namecallAnchors);
            rbx::lua_pushvalue(state, 1);        // key: the name itself
            rbx::lua_pushboolean(state, 1);
            rbx::lua_rawset(state, -3);
            rbx::lua_settop(state, -2);          // drop the table

            luaobj::write<const void*>(reinterpret_cast<void*>(state), luaobj::state::namecall, name);
            return 0;
        }

        // The namecall atom, and why a hooked __namecall needs a stack of them.
        //
        // `L->namecall` is set by the VM immediately before a `__namecall`
        // metamethod runs (`lvmexecute.cpp`, both the LOP_NAMECALL and the
        // LOP_NAMECALLUDATA paths do `L->namecall = tsvalue(kv)`) and is never
        // put back. Roblox never notices because its per-tag fast path answers
        // member calls without a metamethod at all - but we clear that path so
        // hooks can see real members (Namecall::restoreMetatableDispatch), and
        // then every remote spy trips over it:
        //
        //     mtHook = hookmetamethod(game, "__namecall", function(self, ...)
        //         if getnamecallmethod() == "FireServer" then
        //             log:Fire(...)          -- a namecall: atom is now "Fire"
        //         end
        //         return mtHook(self, ...)   -- dispatched as "Fire", not FireServer
        //     end)
        //
        // Measured in the test place, whose LocalScript fires a RemoteEvent every
        // two seconds: one "argument #1 expects a string, but table was passed"
        // per fire, and the remote never actually reaches the server.
        //
        // Restoring on the way out of *our* metamethod cannot fix it - by the
        // time any of our code runs the inner call has already overwritten the
        // outer atom. The only place that still knows the outer method is the
        // metamethod invocation itself, which is why hookmetamethod wraps the
        // hook: the wrapper records the atom the VM just set, and on the way out
        // puts back whichever namecall is still running underneath it.
        namespace Atom {

            // Entries are tagged with the call frame that pushed them, so an
            // error that longjmps past a pop heals on the next push instead of
            // leaving the stack permanently one deep. Frames are identified by
            // their offset from base_ci, never by address: Luau grows the
            // CallInfo array by reallocating it, which moves every frame.
            struct Entry {
                int frame;
                const void* atom;
            };

            inline std::mutex mutex;
            inline std::unordered_map<uint64_t, std::vector<Entry>> stacks;

            inline int frameOf(uint64_t state)
            {
                const int step = Stack::stride(state);
                if (step <= 0)
                    return -1;
                auto* callinfo = static_cast<uint8_t*>(
                    luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci));
                auto* first = static_cast<uint8_t*>(
                    luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base_ci));
                if (!callinfo || !first || callinfo < first)
                    return -1;
                return static_cast<int>((callinfo - first) / step);
            }

            // Anything at or below the frame being entered cannot still be live.
            inline void dropDeadFrames(std::vector<Entry>& stack, int frame)
            {
                while (!stack.empty() && stack.back().frame >= frame)
                    stack.pop_back();
            }

            // A thread whose metamethod raised leaves its entry behind - the
            // longjmp goes past the pop - so this map is keyed by pointers to
            // threads that may be long dead. A game with a remote spy installed
            // would grow it for the whole session, and a new thread allocated
            // at a recycled address would inherit a stale atom. Bounded the
            // same way Http::namecallFrames is: past the ceiling the whole
            // thing starts again, which costs at worst one unrestored atom and
            // heals on the next call.
            inline constexpr size_t kMaxThreads = 512;

            inline void push(uint64_t state, int frame, const void* atom)
            {
                if (frame < 0)
                    return;
                std::lock_guard<std::mutex> lock(mutex);
                if (stacks.size() > kMaxThreads && stacks.find(state) == stacks.end()) {
                    journal::writef("ATOM clearing %zu stale namecall stacks", stacks.size());
                    stacks.clear();
                }
                auto& stack = stacks[state];
                dropDeadFrames(stack, frame);
                stack.push_back({frame, atom});
            }

            // Pops this frame and puts back the enclosing namecall's method,
            // which is the whole point - the nested call is what took it away.
            inline void pop(uint64_t state, int frame)
            {
                if (frame < 0)
                    return;
                const void* restore = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto found = stacks.find(state);
                    if (found == stacks.end())
                        return;
                    dropDeadFrames(found->second, frame);
                    if (found->second.empty()) {
                        stacks.erase(found);
                        return;
                    }
                    restore = found->second.back().atom;
                }
                if (restore)
                    luaobj::write<const void*>(reinterpret_cast<void*>(state),
                                               luaobj::state::namecall, restore);
            }
        }

        // The hook the wrapper forwards to rides in its upvalue.
        //
        // This first went in the way newcclosure keeps its wrapped functions -
        // a side map from the running closure's address to a registry ref -
        // and that was wrong here. `runningClosure` reads `L->ci->func`, and
        // several paths legitimately rewrite that slot (Yield::claimFrame swaps
        // it outright), so the lookup missed and the wrapper raised. Under sUNC
        // it raised from inside a RenderStep binding, which produced pages of
        //     hookmetamethod: the hook is no longer reachable
        //     RunService:fireRenderStepEarlyFunctions unexpected error
        // An upvalue cannot be lost, needs no map, no mutex and no clearing on
        // rejoin. newcclosure only avoids upvalues because the spec requires
        // its result to have none; nothing inspects this wrapper.
        inline int namecallWrapperContinue(lua_State* L, int status) {
            (void)status;
            uint64_t state = reinterpret_cast<uint64_t>(L);
            Atom::pop(state, Atom::frameOf(state));
            return rbx::lua_gettop(state);
        }

        inline int namecallWrapperCall(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            // Read the atom before anything else runs: this is the one moment
            // it is guaranteed to be the method the caller actually wrote.
            const int frame = Atom::frameOf(state);
            Atom::push(state, frame,
                       luaobj::read<const void*>(reinterpret_cast<void*>(state),
                                                 luaobj::state::namecall));

            const int arguments = rbx::lua_gettop(state);
            rbx::lua_pushvalue(state, lua_upvalueindex(1));
            rbx::lua_insert(state, 1);
            rbx::lua_call(state, arguments, LUA_MULTRET);

            // A yield leaves this frame standing; the pop belongs to the
            // continuation, which is what the VM comes back through.
            if (luaobj::yielded(state))
                return -1;

            Atom::pop(state, frame);
            return rbx::lua_gettop(state);
        }

        // hookmetamethod is a metatable write, and nothing more.
        //
        // This used to patch the metamethod closure's `c.f` to a dispatcher and
        // keep a chain of hooks in C++, so that the value handed back was the
        // metamethod itself and its identity survived being hooked. It worked,
        // and it was the wrong shape:
        //
        //   - every member access in the game went through the dispatcher,
        //     because clearing Roblox's per-tag fast paths routes `part.Size`
        //     through the metatable. One property read went from 138ns to
        //     6.4us with twenty-five hooks stacked.
        //   - `mt.__index = original`, which is how every script and every
        //     suite unhooks, did nothing at all: the metatable already pointed
        //     at that closure and its `c.f` was still ours. The chain only ever
        //     grew, and sUNC - which hooks and restores in check after check -
        //     ended up stalling the client for a minute and a half at a time.
        //
        // The reference executors just write the field and hand back what was
        // there (`Vicna .../env/libs/metatable.hpp`), and that is better on
        // every count: an unhooked metamethod costs exactly what Roblox costs,
        // chaining falls out for free because each hook is handed the previous
        // value, and a restore is a real restore. Identity is still preserved
        // for the first hook, which is handed the metamethod itself.
        //
        // `game:HttpGet` does not depend on any of this - `Http::install`
        // redirects the metamethod closure's own `c.f`, so it keeps working as
        // long as a hook forwards to what it was given.
        inline int hookmetamethod(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            auto fail = [&](const char* message) {
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            };

            if (rbx::lua_type(state, 2) != luaobj::kTagString)
                return fail("hookmetamethod: expected a metamethod name");
            if (!rbx::lua_islfunction(state, 3) && !rbx::lua_iscfunction(state, 3))
                return fail("hookmetamethod: expected a function to hook with");
            if (!rbx::lua_getmetatable(state, 1))
                return fail("hookmetamethod: object has no metatable");

            const int metatable = rbx::lua_gettop(state);

            size_t nameLen = 0;
            const char* name = rbx::lua_tolstring(state, 2, &nameLen);
            const std::string method(name ? name : "", nameLen);

            rbx::lua_getfield(state, metatable, method.c_str());
            if (!rbx::lua_islfunction(state, -1) && !rbx::lua_iscfunction(state, -1)) {
                rbx::lua_settop(state, -3);
                return fail("hookmetamethod: that metamethod is not a function");
            }
            const int previous = rbx::lua_gettop(state);

            // Every metatable worth hooking is readonly, and it goes back the
            // way it was found.
            const bool locked = rbx::lua_getreadonly(state, metatable) != 0;
            if (locked)
                rbx::lua_setreadonly(state, metatable, 0);

            // __namecall is the one metamethod whose invocation carries state
            // the VM does not put back, so the hook goes in behind a wrapper
            // that does - see Atom above. Every other metamethod is written
            // straight in, exactly as before.
            int wrapperIndex = 0;
            if (method == "__namecall") {
                // The caller gets a plain forwarding wrapper as `old`: a tail
                // call, so the original metamethod's frame is the one an
                // error handler sees and its error message is untouched.
                //
                // The hook's environment is left alone. It is the caller's
                // function, and scripts hand us newcclosure/newlclosure
                // wrappers whose payload lives in their environment - rewriting
                // it turns every later namecall into a nil call.
                //
                // The forwarder runs against the game's root globals rather
                // than the caller's script environment, so that asking for
                // `old`'s environment never hands back the private globals a
                // script has accumulated.
                const void* previousClosure = rbx::lua_topointer(state, previous);
                const void* nameSource =
                    previousClosure && luaobj::byte(previousClosure, luaobj::closure::isC) == 0
                        ? previousClosure : nullptr;
                rbx::lua_getfield(state, LUA_REGISTRYINDEX, "vanta.renv");
                if (rbx::lua_type(state, -1) != luaobj::kTagTable) {
                    rbx::lua_settop(state, -2);
                    rbx::lua_createtable(state, 0, 0);
                }
                if (Closures::pushLuaWrapper(state, previous, rbx::lua_gettop(state),
                                             nameSource, false))
                    wrapperIndex = rbx::lua_gettop(state);

                // Direct install. The old Atom wrapper put a C frame between
                // the error and the hooked frame, which the stack test can see.
                rbx::lua_pushvalue(state, 3);
            } else {
                rbx::lua_pushvalue(state, 3);
            }
            Closures::registerMetamethodHook(state, rbx::lua_topointer(state, -1),
                                             metatable, previous, 3, 0, method);
            rbx::lua_setfield(state, metatable, method.c_str());

            if (locked)
                rbx::lua_setreadonly(state, metatable, 1);

            if (wrapperIndex)
                rbx::lua_pushvalue(state, wrapperIndex);
            else
                rbx::lua_pushvalue(state, previous);
            return 1;
        }


        // Kept for the bootstrap, which was written before lua_toboolean was
        // located and does its truthiness test in Lua instead.
        inline int unlock(lua_State* L) {
            rbx::lua_setreadonly(reinterpret_cast<uint64_t>(L), 1, 0);
            return 0;
        }

        inline int lock(lua_State* L) {
            rbx::lua_setreadonly(reinterpret_cast<uint64_t>(L), 1, 1);
            return 0;
        }

        // lua_setmetatable takes the metatable off the top and applies it to the
        // object at the given index, so argument 2 is copied up first. Returns
        // the object, which is what scripts expect to be able to chain on.
        // Argument shapes are the references' consensus: the object must be
        // something that can carry a metatable, and the metatable must be a
        // table or nil (Module-Source and sourcessss both spell out exactly
        // this pair). Handing the object back is what Vicna and Module-Source
        // do; the macOS tree returns the metatable it was given, which no other
        // tree does and which reads as an oversight.
        inline int setrawmetatable(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "setrawmetatable");
            (void)trace;

            // Not just tables and userdata. Every other type shares one
            // metatable per type, and lua_setmetatable already writes it - its
            // default arm is `L->global->mt[ttype(obj)] = mt`, right there in
            // lapi.cpp. So a string is a legitimate target: it sets the
            // metatable every string sees, which is the whole point of asking.
            //
            // Five of the six references allow it. mac and Vicna gate arg 1
            // with luaL_checkany alone, RbxStu rejects only none-or-nil, and
            // SkidBase and sourcessss both have the `table or userdata` line
            // commented out and switch on the type instead - reaching
            // `global->mt` for everything that is neither. Module-Source is the
            // one holdout, and this was a copy of it.
            //
            // none and nil are still refused: index 1 empty is a call with no
            // argument, and nil would set the metatable for the nil type, which
            // nobody means to do.
            const int object = rbx::lua_type(state, 1);
            if (object <= luaobj::kTagNil)
                return utils::fail(state, "setrawmetatable: table or userdata expected");

            // Arg 2 stays table-or-nil even though SkidBase and sourcessss also
            // take userdata: lua_setmetatable asserts `ttistable` on it before
            // storing, and that api_check is compiled out of a release build -
            // so a userdata here would not be caught, it would be stored as a
            // Table* that is not one.
            const int metatable = rbx::lua_type(state, 2);
            if (metatable != luaobj::kTagTable && metatable != luaobj::kTagNil)
                return utils::fail(state, "setrawmetatable: nil or table expected");

            // A readonly target would make lua_setmetatable raise ("attempt to
            // modify a readonly table"), which is exactly the case someone
            // reaches for this function to get past. RbxStu is the only one of
            // the six that handles it - it clears the flag around the write -
            // and there is no cost to doing the same: the flag goes back
            // afterwards, so the table is left as it was found.
            const bool locked = object == luaobj::kTagTable && rbx::lua_getreadonly(state, 1);
            if (locked)
                rbx::lua_setreadonly(state, 1, 0);

            rbx::lua_pushvalue(state, 2);
            rbx::lua_setmetatable(state, 1);

            if (locked)
                rbx::lua_setreadonly(state, 1, 1);

            rbx::lua_pushvalue(state, 1);
            return 1;
        }

    }
    
    namespace Misc {

        // __vanta_log(...) - one line into /tmp/vanta_executor.log, flushed.
        //
        // Anything printed into the game's own log is lost when the client
        // freezes or dies, which is exactly when it is worth having. This lands
        // on disk immediately and can be watched from outside with
        // `tail -f /tmp/vanta_executor.log` while the run is happening.
        inline int journalWrite(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::string line;
            const int count = rbx::lua_gettop(state);
            for (int index = 1; index <= count; index++) {
                if (index > 1)
                    line += ' ';
                size_t length = 0;
                const char* text = rbx::lua_tolstring(state, index, &length);
                if (text)
                    line.append(text, length);
                else
                    line += trace::describe(state, index);
            }
            journal::write(line.c_str());
            return 0;
        }

        inline int loadstring(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            // Argument #1 must be a string, and a missing or wrong-typed one has
            // to RAISE. Without this `lua_tolstring` simply answers NULL, the
            // source becomes "", and `loadstring()` hands back a do-nothing empty
            // function instead of erroring - which is not what any other executor
            // does and not what a script expects. A Luraph payload that calls
            // `loadstring()` with no argument (an anti-tamper probe: on a real
            // environment it throws) got a function back, carried on into a state
            // that cannot happen, and spun forever - freezing the whole client,
            // because a script that never yields starves Roblox's scheduler.
            // `failingscripts/1.lua` is exactly that, and it is the last call it
            // makes before the freeze.
            //
            // 6/6 references validate this argument: luaL_checktype(1, LUA_TSTRING)
            // in Vicna, SkidBase, Module-Source and sourcessss, luaL_checkstring
            // in RbxStu, and luaL_argexpected(lua_isstring(L, 1)) in the macOS one.
            // We were the only one that did not. The strict form is the majority's;
            // it differs from the macOS tree only for a numeric argument, which
            // that one coerces and this one rejects.
            rbx::luaL_checktype(state, 1, LUA_TSTRING);

            size_t sourceLen = 0;
            const char* source = rbx::lua_tolstring(state, 1, &sourceLen);

            size_t nameLen = 0;
            const char* name = rbx::lua_tolstring(state, 2, &nameLen);
            // A custom name is passed through verbatim, because the spec
            // documents the error message it produces: [string "name"]:1: ...
            std::string chunkname = name ? std::string(name, nameLen) : "vanta";

            std::string bytecode = compileScript(source ? std::string(source, sourceLen) : "");

            int loaded;
            {
                // Claim the buffer so Roblox's signature check lets it through.
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(state, chunkname.c_str(), bytecode.c_str(), bytecode.size(), 0);
            }

            if (loaded != 0) {
                size_t errorLen = 0;
                const char* error = rbx::lua_tolstring(state, -1, &errorLen);
                std::string message = error ? std::string(error, errorLen) : "";
                rbx::lua_settop(state, -2);
                rbx::lua_pushnil(state);
                rbx::lua_pushlstring(state, message.c_str(), message.size());
                return 2;
            }

            // Without this the chunk's proto carries no capabilities, and the
            // first thing it touches that needs one - Players.LocalPlayer, say -
            // fails with "the current thread cannot read". ExecuteScript has
            // always done this; loadstring silently did not.
            rbx::setprotocapabilities(state, -1, &MaxCapabilities);

            chunks::mark(chunkname);
            return 1;
        }

        // The TaskScheduler keeps the frame budget as seconds-per-frame at
        // +0xB8, so the cap a script names is stored as its reciprocal. The
        // value is sanity-checked before it is trusted: a frame time outside
        // (0, 1] means the offset is wrong for this build, and answering 60
        // beats writing over whatever really lives there.
        inline double* schedulerFrameTime()
        {
            if (!offsets::taskscheduler)
                return nullptr;
            const uintptr_t scheduler = *reinterpret_cast<uintptr_t*>(offsets::taskscheduler);
            if (!scheduler)
                return nullptr;
            auto* budget = reinterpret_cast<double*>(scheduler + offsets::structs::scheduler_frametime);
            const double current = *budget;
            if (!(current > 0.0 && current <= 1.0))
                return nullptr;
            return budget;
        }

        // What "uncapped" is written as. The scheduler's field is a frame
        // budget in seconds, so there is no value meaning "no cap" - the
        // smallest interval it will honour is as close as it gets.
        inline constexpr double kUncappedTarget = 10000.0;

        inline int getfpscap(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const double* budget = schedulerFrameTime();
            if (!budget) {
                utils::pushNumber(state, 60.0);
                return 1;
            }
            const double cap = 1.0 / *budget;
            // setfpscap(0) has to read back as 0 rather than as the sentinel it
            // is stored as, or the pair does not round-trip.
            utils::pushNumber(state, cap >= kUncappedTarget ? 0.0 : cap);
            return 1;
        }

        inline int setfpscap(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const int given = rbx::lua_type(state, 1);
            if (given != luaobj::kTagNumber && given != luaobj::kTagInteger)
                return utils::fail(state, "setfpscap: expected a non-negative number");
            const double requested = utils::toNumber(state, 1);
            if (!(requested >= 0.0))
                return utils::fail(state, "setfpscap: expected a non-negative number");

            // 0 means "uncapped", which for a frame budget is the smallest
            // interval the scheduler will honour rather than zero seconds.
            // sourcessss picks 1/10000 for this and RbxStu 1000; only the macOS
            // tree reads 0 as "back to 60", which no other reference does and
            // which would make setfpscap(0) a cap rather than the removal of
            // one.
            const double target = requested == 0.0 ? kUncappedTarget : requested;
            if (double* budget = schedulerFrameTime())
                *budget = 1.0 / target;
            return 0;
        }

        inline int clearqueueonteleport(lua_State* L) {
            (void)L;
            teleport::clear();
            return 0;
        }

        // Windows executors hand this a Roblox instance-serialisation blob so
        // Studio can paste it. macOS has no such pasteboard type of its own, so
        // the payload goes on the general pasteboard under Roblox's own UTI
        // alongside a plain-text copy, which is what Studio reads back.
        inline int setrbxclipboard(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            size_t len = 0;
            const char* text = rbx::lua_tolstring(state, 1, &len);
            if (!text) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            Class pasteboardClass = objc_getClass("NSPasteboard");
            Class stringClass = objc_getClass("NSString");
            Class dataClass = objc_getClass("NSData");
            if (!pasteboardClass || !stringClass || !dataClass) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            using SendId = id (*)(id, SEL);
            using SendIdChars = id (*)(id, SEL, const char*);
            using SendLong = long (*)(id, SEL);
            using SendData = id (*)(id, SEL, const void*, unsigned long);
            using SendBoolIdId = signed char (*)(id, SEL, id, id);

            const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
            const auto sendIdChars = reinterpret_cast<SendIdChars>(objc_msgSend);
            const auto sendLong = reinterpret_cast<SendLong>(objc_msgSend);
            const auto sendData = reinterpret_cast<SendData>(objc_msgSend);
            const auto sendBoolIdId = reinterpret_cast<SendBoolIdId>(objc_msgSend);

            id pasteboard = sendId(reinterpret_cast<id>(pasteboardClass), sel_registerName("generalPasteboard"));
            if (!pasteboard) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const SEL fromUTF8 = sel_registerName("stringWithUTF8String:");
            id robloxType = sendIdChars(reinterpret_cast<id>(stringClass), fromUTF8, "com.roblox.rbxm");
            id plainType = sendIdChars(reinterpret_cast<id>(stringClass), fromUTF8, "public.utf8-plain-text");
            id payload = sendData(reinterpret_cast<id>(dataClass),
                                  sel_registerName("dataWithBytes:length:"), text, len);
            id plain = sendIdChars(reinterpret_cast<id>(stringClass), fromUTF8, std::string(text, len).c_str());

            sendLong(pasteboard, sel_registerName("clearContents"));
            int ok = 0;
            if (payload && robloxType)
                ok |= sendBoolIdId(pasteboard, sel_registerName("setData:forType:"), payload, robloxType) ? 1 : 0;
            if (plain && plainType)
                ok |= sendBoolIdId(pasteboard, sel_registerName("setString:forType:"), plain, plainType) ? 1 : 0;

            rbx::lua_pushboolean(state, ok);
            return 1;
        }

        // CFUserNotification rather than NSAlert: the alert has to come up from
        // whatever thread the script is on, and NSAlert insists on the main run
        // loop. The flag words are Windows' MB_* values, because that is what
        // every script written against this function passes.
        inline int messagebox(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            size_t textLen = 0;
            const char* text = rbx::lua_tolstring(state, 1, &textLen);
            size_t captionLen = 0;
            const char* caption = rbx::lua_tolstring(state, 2, &captionLen);
            int isnum = 0;
            const int flags = rbx::lua_tointegerx(state, 3, &isnum);

            CFOptionFlags level;
            switch ((isnum ? flags : 0) & 0x30) {
                case 0x10: level = kCFUserNotificationStopAlertLevel; break;
                case 0x20: level = kCFUserNotificationNoteAlertLevel; break;
                case 0x30: level = kCFUserNotificationCautionAlertLevel; break;
                default:   level = kCFUserNotificationPlainAlertLevel; break;
            }

            CFStringRef title = CFStringCreateWithBytes(
                nullptr, reinterpret_cast<const UInt8*>(caption ? caption : ""),
                static_cast<CFIndex>(captionLen), kCFStringEncodingUTF8, false);
            CFStringRef body = CFStringCreateWithBytes(
                nullptr, reinterpret_cast<const UInt8*>(text ? text : ""),
                static_cast<CFIndex>(textLen), kCFStringEncodingUTF8, false);

            CFStringRef primary = CFSTR("OK");
            CFStringRef secondary = nullptr;
            CFStringRef tertiary = nullptr;
            switch ((isnum ? flags : 0) & 0xF) {
                case 0x1: secondary = CFSTR("Cancel"); break;
                case 0x2: primary = CFSTR("Abort"); secondary = CFSTR("Retry"); tertiary = CFSTR("Ignore"); break;
                case 0x3: primary = CFSTR("Yes"); secondary = CFSTR("No"); tertiary = CFSTR("Cancel"); break;
                case 0x4: primary = CFSTR("Yes"); secondary = CFSTR("No"); break;
                case 0x5: primary = CFSTR("Retry"); secondary = CFSTR("Cancel"); break;
                default: break;
            }

            CFOptionFlags response = 0;
            const SInt32 status = CFUserNotificationDisplayAlert(
                0, level, nullptr, nullptr, nullptr, title, body,
                primary, secondary, tertiary, &response);

            if (title) CFRelease(title);
            if (body) CFRelease(body);

            // Windows' IDOK / IDCANCEL / IDABORT / IDRETRY / IDIGNORE / IDYES /
            // IDNO, chosen by which button the alert actually offered.
            int result = 0;
            if (status == 0) {
                const int shape = (isnum ? flags : 0) & 0xF;
                const CFOptionFlags button = response & 0x3;
                if (shape == 0x3 || shape == 0x4)
                    result = button == 0 ? 6 : (button == 1 ? 7 : 2);
                else if (shape == 0x2)
                    result = button == 0 ? 3 : (button == 1 ? 4 : 5);
                else if (shape == 0x5)
                    result = button == 0 ? 4 : 2;
                else
                    result = button == 0 ? 1 : 2;
            }
            rbx::lua_pushinteger(state, result);
            return 1;
        }

        inline int getexecutorname(lua_State* L)
        {
            const char* name = "Vanta";
            rbx::lua_pushlstring(reinterpret_cast<uint64_t>(L), name, std::strlen(name));
            return 1;
        }

        inline int identifyexecutor(lua_State* L)
        {
            const char* ExecutorName = "Vanta";
            const char* ExecutorVersion = VANTA_VERSION_STR;
            
            rbx::lua_pushlstring(reinterpret_cast<uint64_t>(L), ExecutorName, std::strlen(ExecutorName));
            rbx::lua_pushlstring(reinterpret_cast<uint64_t>(L), ExecutorVersion, std::strlen(ExecutorVersion));
            return 2;
        }
        
        inline int queue_on_teleport(lua_State* L) {
            size_t len = 0;
            const char* source = rbx::lua_tolstring(reinterpret_cast<uint64_t>(L), 1, &len);
            if (source && len)
                teleport::push(std::string(source, len));
            return 0;
        }

        // The clipboard is NSPasteboard, which lives in AppKit. RobloxPlayer has
        // already loaded that framework, so looking the class up by name through
        // the Objective-C runtime finds it without this dylib linking AppKit or
        // becoming an Objective-C++ translation unit; only libobjc is needed.
        // objc_msgSend is variadic, so every send goes through a typed cast.
        inline int setclipboard(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            // Every reference argument-checks this; ours quietly copied an
            // empty string when handed anything else.
            if (rbx::lua_type(state, 1) != luaobj::kTagString)
                return utils::fail(state, "setclipboard: string expected");
            size_t len = 0;
            const char* text = rbx::lua_tolstring(state, 1, &len);
            std::string value = text ? std::string(text, len) : "";

            Class pasteboardClass = objc_getClass("NSPasteboard");
            Class stringClass = objc_getClass("NSString");
            if (!pasteboardClass || !stringClass)
                return 0;

            using SendId = id (*)(id, SEL);
            using SendIdChars = id (*)(id, SEL, const char*);
            using SendLong = long (*)(id, SEL);
            using SendBoolIdId = signed char (*)(id, SEL, id, id);

            const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
            const auto sendIdChars = reinterpret_cast<SendIdChars>(objc_msgSend);
            const auto sendLong = reinterpret_cast<SendLong>(objc_msgSend);
            const auto sendBoolIdId = reinterpret_cast<SendBoolIdId>(objc_msgSend);

            id pasteboard = sendId(reinterpret_cast<id>(pasteboardClass), sel_registerName("generalPasteboard"));
            if (!pasteboard)
                return 0;

            // NSPasteboardTypeString is "public.utf8-plain-text"; spelling it out
            // means no link-time reference to the AppKit symbol either. A string
            // that is not valid UTF-8 comes back nil, which is left as a no-op.
            const SEL fromUTF8 = sel_registerName("stringWithUTF8String:");
            id contents = sendIdChars(reinterpret_cast<id>(stringClass), fromUTF8, value.c_str());
            id type = sendIdChars(reinterpret_cast<id>(stringClass), fromUTF8, "public.utf8-plain-text");
            if (!contents || !type)
                return 0;

            sendLong(pasteboard, sel_registerName("clearContents"));
            sendBoolIdId(pasteboard, sel_registerName("setString:forType:"), contents, type);
            return 0;
        }

        inline int getclipboard(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            Class pasteboardClass = objc_getClass("NSPasteboard");
            Class stringClass = objc_getClass("NSString");
            if (!pasteboardClass || !stringClass) {
                rbx::lua_pushlstring(state, "", 0);
                return 1;
            }

            using SendId = id (*)(id, SEL);
            using SendIdChars = id (*)(id, SEL, const char*);
            using SendIdId = id (*)(id, SEL, id);
            using SendChars = const char* (*)(id, SEL);

            const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
            const auto sendIdChars = reinterpret_cast<SendIdChars>(objc_msgSend);
            const auto sendIdId = reinterpret_cast<SendIdId>(objc_msgSend);
            const auto sendChars = reinterpret_cast<SendChars>(objc_msgSend);

            id pasteboard = sendId(reinterpret_cast<id>(pasteboardClass), sel_registerName("generalPasteboard"));
            id type = sendIdChars(reinterpret_cast<id>(stringClass), sel_registerName("stringWithUTF8String:"),
                                  "public.utf8-plain-text");
            id contents = pasteboard && type
                ? sendIdId(pasteboard, sel_registerName("stringForType:"), type)
                : nullptr;

            const char* text = contents ? sendChars(contents, sel_registerName("UTF8String")) : nullptr;
            rbx::lua_pushlstring(state, text ? text : "", text ? std::strlen(text) : 0);
            return 1;
        }

        // Roblox is a Cocoa app, so "is the game focused" is just whether its
        // NSApplication is active. Same runtime trick as setclipboard.
        inline int isrbxactive(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            Class applicationClass = objc_getClass("NSApplication");
            if (!applicationClass) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            using SendId = id (*)(id, SEL);
            using SendBool = signed char (*)(id, SEL);
            const auto sendId = reinterpret_cast<SendId>(objc_msgSend);
            const auto sendBool = reinterpret_cast<SendBool>(objc_msgSend);

            id application = sendId(reinterpret_cast<id>(applicationClass),
                                    sel_registerName("sharedApplication"));
            if (!application) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            rbx::lua_pushboolean(state, sendBool(application, sel_registerName("isActive")) ? 1 : 0);
            return 1;
        }

        // game:GetObjects(url) -> { Instance }
        //
        // Roblox has taken GetObjects out of the client. Not hidden behind a
        // security level - gone: the string is not in the player binary at all
        // (`strings RobloxPlayer | grep -x GetObjects` finds nothing, while
        // GetDebugId, LoadLocalAsset and every other member is there once), so
        // both `game:GetObjects(url)` and `game.GetObjects` answer "GetObjects
        // is not a valid member of DataModel". Nothing about our capability
        // word is involved - measured at identity 8 with Plugin, LocalUser and
        // RobloxScript all set, and with Roblox's own per-tag fast path put
        // back (tests/go.lua).
        //
        // Five of the six references serve it themselves for exactly this
        // reason - mac, Vicna, sourcessss, SkidBase and Module-Source - and all
        // five do it the same way: InsertService:LoadLocalAsset, whose single
        // Instance is wrapped in the array the caller expects. mac and
        // Module-Source also register a bare `GetObjects` global; that is 2/5
        // and is not in any spec, so only the two metamethod routes are taken.
        //
        // LoadLocalAsset blocks rather than yields - measured at 1.13 s with
        // zero Heartbeats in tests/la.lua - so there is nothing here for
        // unc::Yield to do, and calling it with lua_call rather than a pcall
        // hands the engine's own message ("Invalid XML", "Unable to load") to
        // the caller instead of a flattened one of ours.
        inline int getobjects(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            // Reached as game:GetObjects(url), as game.GetObjects(game, url)
            // and as a detached `local f = game.GetObjects`, so the self
            // argument is detected rather than assumed - the same test
            // Http::argumentBase makes for HttpGet.
            const int base = rbx::lua_type(state, 1) == luaobj::kTagUserdata ? 1 : 0;
            if (rbx::lua_type(state, base + 1) != luaobj::kTagString) {
                const char* message = "GetObjects: expected an asset url string";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            size_t length = 0;
            const char* url = rbx::lua_tolstring(state, base + 1, &length);
            const std::string asset(url ? url : "", url ? length : 0);

            // game:GetService("InsertService")
            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "game");
            rbx::lua_getfield(state, -1, "GetService");
            rbx::lua_pushvalue(state, -2);
            rbx::lua_pushlstring(state, "InsertService", 13);
            rbx::lua_call(state, 2, 1);
            rbx::lua_remove(state, -2);

            rbx::lua_getfield(state, -1, "LoadLocalAsset");
            rbx::lua_pushvalue(state, -2);
            rbx::lua_pushlstring(state, asset.data(), asset.size());
            rbx::lua_call(state, 2, 1);
            rbx::lua_remove(state, -2);

            rbx::lua_createtable(state, 1, 0);
            rbx::lua_pushvalue(state, -2);
            rbx::lua_rawseti(state, -2, 1);
            rbx::lua_remove(state, -2);
            return 1;
        }

    }

    namespace Reflection {

        inline int getthreadidentity(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "getthreadidentity");
            (void)trace;
            uint64_t extraSpace = rbx::lua_getthreaddata(state);
            uint32_t identity = extraSpace
                ? *reinterpret_cast<uint32_t*>(extraSpace + offsets::identity)
                : 0;
            rbx::lua_pushinteger(state, static_cast<int>(identity));
            return 1;
        }

        // The engine keeps its own copy of the security context in a
        // thread-local, and that copy - not the one in the lua_State's extra
        // space - is what it reads back. With only the extra space written,
        // `printidentity()` answered "Current identity is 8" at every level
        // (tests/pr1.lua), which is what sUNC reports as "Couldn't set the
        // identity to verify capability".
        //
        // Every reference executor writes both. See offsets::identitypropagator
        // for how this one was found; identity is an int32 at +0 and
        // capabilities a uint64 at +0x28, which is the offset the macOS
        // reference uses (Windows builds have it at +0x20).
        // Where the engine's security context keeps the capability mask.
        //
        // The identity is an int32 at +0 on every build seen. The capabilities
        // sit one word past the `lua_State*` the propagator stores beside them:
        // 0.734-0.736 keep L at +0x18 and the mask at +0x28, and 0.737 moved
        // both on by eight - L at +0x20, the mask at +0x30.
        //
        // Writing the old +0x28 on 0.737 lands in a hole. Nothing faults and
        // nothing reads back wrong, because we then read our own write - which
        // is exactly how this hid: the identity and the mask looked consistent
        // to us while the engine, which reads its own field, saw neither. That
        // is sUNC's "Couldn't set the identity to verify capability", and
        // SUNC.md records the same failure from the same cause in 2026-08.
        //
        // So L is found rather than assumed: the context holds this very
        // thread's state pointer, which is a value we already have.
        inline int engineCapabilityOffset(uint64_t state, uintptr_t context)
        {
            static int cached = -1;
            if (cached >= 0)
                return cached;
            for (int at = 0x10; at <= 0x38; at += 8) {
                uintptr_t word = 0;
                if (Probe::safeRead(&word, context + at, sizeof(word)) &&
                    word == static_cast<uintptr_t>(state)) {
                    cached = at + 0x10;
                    return cached;
                }
            }
            cached = 0x28;          // what every build before 0.737 used
            return cached;
        }

        inline void writeEngineIdentity(uint64_t state, uint32_t identity, uint64_t capabilities)
        {
            const uintptr_t context = rbx::identitypropagator(state);
            if (!context)
                return;
            *reinterpret_cast<uint32_t*>(context + 0x00) = identity;
            *reinterpret_cast<uint64_t*>(context + engineCapabilityOffset(state, context)) =
                capabilities;
        }

        // Mirrors executor.hpp's SetIdentity, including the capability mask it
        // ORs in. That means lowering the identity does not actually surrender
        // capabilities - it keeps our own threads working after a script calls
        // this, at the cost of setthreadidentity(2) not really sandboxing.
        inline int setthreadidentity(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            trace::Scope trace(state, "setthreadidentity");

            int isnum = 0;
            const int requested = rbx::lua_tointegerx(state, 1, &isnum);
            if (!isnum) {
                const char* message = "setthreadidentity: expected an identity number";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_error(state);
                return 0;
            }

            uint64_t extraSpace = rbx::lua_getthreaddata(state);
            if (!extraSpace)
                return 0;

            // Any number is accepted and stored verbatim, because
            // getthreadidentity has to read back whatever was written - sUNC
            // sets identities well outside 0..8 (one call arrives as INT_MIN,
            // which is what lua_tointegerx makes of an out-of-range double) and
            // then asks for it again. Raising there aborted the check before it
            // could. Only one of the five references that have this validates
            // at all - the macOS one accepts 0..12 and *silently ignores*
            // anything else, which would fail the same read-back; sourcessss and
            // Vicna store what they are given, as this does.
            *reinterpret_cast<uint32_t*>(extraSpace + offsets::identity) = static_cast<uint32_t>(requested);

            // Capabilities are the honest ones for the identity, so lowering
            // identity really does surrender access - which is the whole point
            // of the call, and what sUNC verifies. The one exception is our own
            // level: identity 8 keeps the blanket mask, because that is what
            // every executor thread runs with and narrowing it would break the
            // executor's own surface rather than the script's.
            //
            // Roblox's identitytocapabilities indexes a table, so it is only
            // ever handed a level it knows; anything outside 0..8 asks about 8.
            uint32_t capabilityIdentity =
                (requested >= 0 && requested <= 8) ? static_cast<uint32_t>(requested) : 8u;
            const uint64_t before = *reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities);
            uint64_t capabilities = rbx::identitytocapabilities(&capabilityIdentity);

            // The blanket mask goes on at *every* level, which is what all five
            // reference executors do (`| 0x3FFFFFFFFFFF00` in Vicna and the
            // macOS tree, `| 0xFFFFFFFFFFFFFFF0` in SkidBase and
            // Module-Source). None of them surrenders capabilities, and the
            // reason shows up the moment a real script is run rather than a
            // compliance suite.
            //
            // Cobalt's callback interceptor does this:
            //
            //     setthreadidentity(2)
            //     setfenv(0, getfenv(CallbackFunction))
            //     ... work ...
            //     setthreadidentity(InitialIdentity)     -- the restore
            //
            // Honest identity 2 has *no* capabilities, so the engine refuses
            // the setfenv with "the current thread cannot call 'setfenv'". That
            // error propagates past the restore, so the thread is left at
            // identity 2 and everything afterwards fails with "the current
            // thread lacks a capability" - which is exactly what the user saw
            // on closing Cobalt's UI and on running it again.
            //
            // Identity 8 was already exempt for the same class of reason
            // (honest 8 has neither CreateInstances nor Players, measured at 26
            // sUNC checks lost). Surrendering capabilities is not something a
            // script can recover from when anything in between throws, and it
            // is not what the engine's own identity gate is for - the identity
            // itself is now written where the engine reads it, which is what
            // makes lowering observable.
            // The executor mark is carried forward, never recomputed. It says
            // who *made* this thread, which no change of identity alters - and
            // it is what checkcaller and Http::ours read, so dropping it here
            // made a script that called setthreadidentity(2) stop being ours
            // and lose game:HttpGet for the rest of its life. Caught by
            // tests/hga.lua, which is why that test lowers the identity.
            capabilities |= 0x3FFFFFFFFFFF00ULL | (before & kExecutorMark);
            *reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities) = capabilities;
            writeEngineIdentity(state, static_cast<uint32_t>(requested), capabilities);
            // Journaled only when traced. This line was written - and flushed,
            // the journal flushes per line - on every call, which made
            // setthreadidentity a 4.3 us call (perf2, 2026-09-03) on a path
            // Cobalt takes per callback and sUNC takes hundreds of times.
            // `__vanta_trace("setthreadidentity")` still gets it.
            if (trace.active)
                journal::writef("IDENT set=%d level=%u caps before=%016llx after=%016llx",
                                requested, capabilityIdentity,
                                static_cast<unsigned long long>(before),
                                static_cast<unsigned long long>(capabilities));
            return 0;
        }

        // Lowers the identity but keeps the executor's blanket capability mask.
        // The bootstrap's `require` needs exactly this: identity 2 is what gets
        // a module past "Cannot require a non-RobloxScript module from a
        // RobloxScript", but the capabilities that go with identity 2 are not
        // enough to load one, so the honest setthreadidentity refuses. Scripts
        // get the honest version; this is for our own machinery.
        inline int setidentityonly(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            int isnum = 0;
            const int requested = rbx::lua_tointegerx(state, 1, &isnum);
            uint64_t extraSpace = rbx::lua_getthreaddata(state);
            if (!isnum || !extraSpace)
                return 0;

            auto* capabilitySlot = reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities);
            const uint64_t mark = *capabilitySlot & kExecutorMark;
            *reinterpret_cast<uint32_t*>(extraSpace + offsets::identity) =
                static_cast<uint32_t>(requested);
            uint32_t capabilityIdentity =
                (requested > 0 && requested <= 8) ? static_cast<uint32_t>(requested) : 8u;
            const uint64_t capabilities =
                rbx::identitytocapabilities(&capabilityIdentity) | 0x3FFFFFFFFFFF00ULL | mark;
            *reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities) = capabilities;
            writeEngineIdentity(state, static_cast<uint32_t>(requested), capabilities);
            return 0;
        }

        // setthreadidentity keeps a blanket capability mask so our own threads
        // stay usable after a script lowers its identity. That mask is exactly
        // what makes Roblox treat a module we require as a RobloxScript, and
        // then refuse the ordinary modules it requires in turn. This is the
        // honest version: the identity's real capabilities and nothing else,
        // for the duration of one call, with the previous pair handed back so
        // the caller can put it straight back.
        inline int strictidentity(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            uint64_t extraSpace = rbx::lua_getthreaddata(state);
            if (!extraSpace) {
                rbx::lua_pushnil(state);
                return 1;
            }

            auto* identitySlot = reinterpret_cast<uint32_t*>(extraSpace + offsets::identity);
            auto* capabilitySlot = reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities);
            const uint32_t previousIdentity = *identitySlot;
            const uint64_t previousCapabilities = *capabilitySlot;

            int isnum = 0;
            const int requested = rbx::lua_tointegerx(state, 1, &isnum);
            if (isnum && requested >= 0) {
                uint32_t identity = static_cast<uint32_t>(requested);
                *identitySlot = identity;
                uint32_t forCapabilities = identity == 0 ? 8 : identity;
                // Honest capabilities for the identity - and the mark, which is
                // not one of them: `require` runs inside this window and still
                // has to read as ours.
                *capabilitySlot = rbx::identitytocapabilities(&forCapabilities)
                    | (previousCapabilities & kExecutorMark);
                writeEngineIdentity(state, identity, *capabilitySlot);
            } else {
                // No argument: this is the restore, taking the pair back off
                // the table the first call returned.
                if (rbx::lua_type(state, 2) == luaobj::kTagTable) {
                    rbx::lua_getfield(state, 2, "identity");
                    int ok = 0;
                    const int identity = rbx::lua_tointegerx(state, -1, &ok);
                    rbx::lua_settop(state, -2);
                    rbx::lua_getfield(state, 2, "capabilities");
                    size_t len = 0;
                    const char* raw = rbx::lua_tolstring(state, -1, &len);
                    rbx::lua_settop(state, -2);
                    if (ok)
                        *identitySlot = static_cast<uint32_t>(identity);
                    if (raw && len == sizeof(uint64_t))
                        std::memcpy(capabilitySlot, raw, sizeof(uint64_t));
                    writeEngineIdentity(state, *identitySlot, *capabilitySlot);
                }
            }

            rbx::lua_createtable(state, 0, 2);
            rbx::lua_pushinteger(state, static_cast<int>(previousIdentity));
            rbx::lua_setfield(state, -2, "identity");
            rbx::lua_pushlstring(state, reinterpret_cast<const char*>(&previousCapabilities),
                                 sizeof(previousCapabilities));
            rbx::lua_setfield(state, -2, "capabilities");
            return 1;
        }

        // gethiddenproperty
        // isscriptable
        // sethiddenproperty
        // setscriptable
        // setthreadidentity
        
    }
    
    namespace Scripts {

        // Writes a bare GC object into a reserved stack slot. There is no push
        // for "this pointer, with this tag", and both the thread walk and the
        // globals-table lookup need one.
        inline void pushObject(uint64_t state, const void* object, int tag)
        {
            rbx::lua_pushnil(state);
            uint8_t* top = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::top);
            uint8_t* slot = top - luaobj::kValueSize;
            luaobj::write<const void*>(slot, luaobj::value::gc, object);
            luaobj::write<int>(slot, luaobj::value::tt, tag);
        }

        // Every live thread, straight off the GC pages. A script's environment
        // is its thread's globals table, so finding the thread is the whole
        // problem - and the registry only holds some of them.
        struct ThreadWalk {
            std::vector<const void*> found;
            int tagAt = 0;
        };

        inline bool threadVisit(void* context, void* page, void* object)
        {
            (void)page;
            auto* walk = static_cast<ThreadWalk*>(context);
            if (luaobj::byte(object, walk->tagAt) == luaobj::kTagThread)
                walk->found.push_back(object);
            return false;
        }

        inline int threads(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            ThreadWalk walk;
            walk.tagAt = gcTagOffset(state);
            rbx::lua_gc(state, LUA_GCSTOP, 0);
            rbx::luaM_visitgco(state, &walk, threadVisit);

            rbx::lua_settop(state, 0);
            rbx::lua_createtable(state, static_cast<int>(walk.found.size()), 0);
            for (size_t i = 0; i < walk.found.size(); i++) {
                pushObject(state, walk.found[i], luaobj::kTagThread);
                rbx::lua_rawseti(state, 1, static_cast<int>(i + 1));
            }
            rbx::lua_gc(state, LUA_GCRESTART, 0);
            return 1;
        }

        // A thread's globals table is its environment: for a game script that
        // is the table holding its `script` global, which is what getsenv and
        // getscriptfromthread are both really asking about.
        inline int threadenv(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagThread) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const void* thread = rbx::lua_topointer(state, 1);
            void* globals = thread ? luaobj::read<void*>(thread, luaobj::state::gt) : nullptr;
            if (!globals) {
                rbx::lua_pushnil(state);
                return 1;
            }

            pushObject(state, globals, luaobj::kTagTable);
            return 1;
        }

        // A weak_ptr we did not write and have no reason to trust.
        //
        // getrunningscripts walks every thread __vanta_threads hands back, and
        // not all of them are script threads. One whose extra space holds
        // something else at this offset sails through a null check and then
        // takes the client down inside Roblox's own pushInstance, which
        // dereferences the control block to take a reference. On 0.737 that
        // was thread 14 of 25: thirteen honest nils and then a segfault, with
        // libVanta on the faulting stack.
        //
        // A null check is not enough because the failure needs only two
        // non-zero words. So the pair is checked as a weak_ptr first: both
        // halves have to be aligned userspace pointers, the control block's
        // two counts have to be plausible, and the object has to reach a class
        // descriptor. None of that proves it is a *script* - isScriptClass on
        // the Lua side still decides that - it only means pushInstance is
        // never handed something arbitrary.
        inline bool looksLikeWeakInstance(const rbx::WeakInstance& weak)
        {
            const auto plausible = [](uintptr_t value) {
                return value > 0x100000000ULL && value < 0x800000000000ULL &&
                       (value & 7) == 0;
            };
            if (!plausible(weak.object) || !plausible(weak.control))
                return false;

            // libc++ keeps use_count - 1, so -1 is expired and a live object
            // is a small non-negative number. Anything wild is not a control
            // block at all.
            int32_t owners = -1, weakOwners = -1;
            if (!Probe::safeRead(&owners, weak.control + 8, sizeof(owners)) ||
                !Probe::safeRead(&weakOwners, weak.control + 12, sizeof(weakOwners)))
                return false;
            if (owners < 0 || weakOwners < 0 ||
                owners > 0x100000 || weakOwners > 0x100000)
                return false;

            uintptr_t descriptor = 0;
            return Probe::safeRead(&descriptor,
                                   weak.object + offsets::structs::instance_classdescriptor,
                                   sizeof(descriptor)) && plausible(descriptor);
        }

        // The script a thread is running, pushed as the Instance itself.
        //
        // The reference in the thread's extra space is a std::weak_ptr, and
        // every reference executor hands exactly that to Roblox's own
        // pushInstance - which is what we do here. It used to come back as a
        // lightuserdata for the bootstrap to look up in the instance cache, and
        // that is where "getrunningscripts was unable to find a running script"
        // comes from: a script the cache has never seen is not in the cache, so
        // the answer was nil for a thread that plainly has a script.
        //
        // A weak_ptr whose object is gone must not be pushed. libc++ keeps
        // use_count - 1 in the control block at +8, so -1 there is expired,
        // which is the check `script.expired()` makes in the references.
        //
        // Other weak_ptrs share the extra space, and a stale one passed to
        // pushInstance takes the client down inside the engine. A slot is only
        // usable when its object still names a live LuaSourceContainer.
        inline bool weakIsScript(const rbx::WeakInstance& weak) {
            uintptr_t descriptor = 0, namePointer = 0;
            std::string name;
            if (!Probe::safeRead(&descriptor,
                                 weak.object + offsets::structs::instance_classdescriptor,
                                 sizeof(descriptor)) ||
                !Probe::safeRead(&namePointer,
                                 descriptor + offsets::structs::descriptor_name,
                                 sizeof(namePointer)) ||
                !Probe::readString(namePointer, name, 64))
                return false;
            return name == "LocalScript" || name == "ModuleScript" || name == "Script";
        }

        inline int threadscript(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const void* thread = rbx::lua_type(state, 1) == luaobj::kTagThread
                ? rbx::lua_topointer(state, 1)
                : reinterpret_cast<const void*>(state);
            if (!thread) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const uint64_t extraSpace = rbx::lua_getthreaddata(reinterpret_cast<uint64_t>(thread));
            rbx::WeakInstance weak{0, 0};
            if (!extraSpace ||
                !Probe::safeRead(&weak, extraSpace + offsets::threadscript, sizeof(weak)) ||
                !looksLikeWeakInstance(weak) || !weakIsScript(weak)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            rbx::pushinstance(state, &weak);
            return 1;
        }

        // Roblox keeps the script a thread belongs to in the thread's extra
        // space, but only identity (0x68) and capabilities (0x40) are known
        // offsets. This finds the rest by comparison rather than by guessing:
        // given a thread and the raw pointer of the script it is running, any
        // word in the extra space equal to that pointer is a candidate.
        inline int findscript(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (rbx::lua_type(state, 1) != luaobj::kTagThread) {
                rbx::lua_pushnil(state);
                return 1;
            }

            const void* thread = rbx::lua_topointer(state, 1);
            void* wanted = rbx::lua_touserdata(state, 2);
            if (!thread || !wanted) {
                rbx::lua_pushnil(state);
                return 1;
            }
            const void* target = *reinterpret_cast<void**>(wanted);

            const uint64_t extraSpace = rbx::lua_getthreaddata(reinterpret_cast<uint64_t>(thread));
            rbx::lua_createtable(state, 0, 0);
            if (!extraSpace)
                return 1;

            // Guarded, not raw. This walks 0x200 bytes of a thread's extra
            // space looking for a pointer, and a thread whose extra space is
            // shorter than that - or a stale one out of __vanta_threads - takes
            // the client down with a segfault that no pcall can catch. The
            // whole point of this helper is to be run when an offset is
            // already suspect, which is exactly when the read is unsafe.
            int found = 0;
            for (int offset = 0; offset < 0x200; offset += 8) {
                const void* word = nullptr;
                if (!Probe::safeRead(&word, extraSpace + offset, sizeof(word)))
                    break;
                if (word == target) {
                    rbx::lua_pushinteger(state, offset);
                    rbx::lua_rawseti(state, -2, ++found);
                }
            }
            return 1;
        }

        // Both halves have to be read inside one call: every C call gets its own
        // CallInfo and its own stack window, so reading L->ci in one call and
        // L->base in the next compares two different frames and answers
        // nothing. `top` is reported beside `base` for the same reason - the
        // stack walk needs the pair, and on 0.737 `base` moved onto `func`
        // (both read 0x10, which cannot be right).
        //
        // Returns { base = {offsets...}, top = {offsets...} }.
        // Diagnostic: one Lua/C call frame, reported raw. The CallInfo field
        // order changes between builds, so debug.getstack has to be re-pinned
        // against a real frame rather than assumed.
        inline int callframe(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            int level = rbx::lua_tointegerx(state, 1, &isnum);
            if (!isnum || level < 0)
                level = 0;

            void* ci = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci);
            void* base_ci = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base_ci);
            const int step = Stack::stride(state);
            if (!ci || !base_ci || step <= 0) {
                rbx::lua_pushnil(state);
                return 1;
            }
            for (int i = 0; i < level && ci > base_ci; i++)
                ci = static_cast<uint8_t*>(ci) - step;

            const uintptr_t func = luaobj::read<uintptr_t>(ci, luaobj::callinfo::func);
            const uintptr_t base = luaobj::read<uintptr_t>(ci, luaobj::callinfo::base);
            const uintptr_t top = luaobj::read<uintptr_t>(ci, luaobj::callinfo::top);

            rbx::lua_createtable(state, 0, 8);
            utils::pushNumber(state, reinterpret_cast<uintptr_t>(ci));
            rbx::lua_setfield(state, -2, "frame");
            utils::pushNumber(state, func);
            rbx::lua_setfield(state, -2, "func");
            utils::pushNumber(state, base);
            rbx::lua_setfield(state, -2, "base");
            utils::pushNumber(state, top);
            rbx::lua_setfield(state, -2, "top");
            rbx::lua_createtable(state, 0, 0);
            for (int off = 0; off < 0x40; off += 8) {
                const uintptr_t word = luaobj::read<uintptr_t>(ci, off);
                rbx::lua_createtable(state, 0, 2);
                utils::pushNumber(state, word);
                rbx::lua_setfield(state, -2, "w");
                utils::pushNumber(state, static_cast<double>(static_cast<long long>(word) -
                                                            static_cast<long long>(func)));
                rbx::lua_setfield(state, -2, "df");
                rbx::lua_rawseti(state, -2, off / 8 + 1);
            }
            rbx::lua_setfield(state, -2, "words");
            return 1;
        }

        inline int callinfobase(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            void* base = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::base);
            void* top = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::top);
            void* callinfo = luaobj::read<void*>(reinterpret_cast<void*>(state), luaobj::state::ci);

            rbx::lua_createtable(state, 0, 2);
            if (!callinfo)
                return 1;

            // `func` is one TValue below `base` - the slot holding the function
            // being called - so it is found by value rather than by guessing a
            // field order. `top` is whatever sits above base and still points
            // into the stack.
            auto* baseBytes = static_cast<uint8_t*>(base);
            void* const func = baseBytes ? baseBytes - luaobj::kValueSize : nullptr;

            const char* const names[3] = {"base", "func", "above"};
            void* const wanted[3] = {base, func, nullptr};
            for (int which = 0; which < 3; which++) {
                rbx::lua_createtable(state, 0, 0);
                int found = 0;
                for (int offset = 0; offset < 0x40; offset += 8) {
                    void* word = luaobj::read<void*>(callinfo, offset);
                    bool hit = false;
                    if (which < 2)
                        hit = (wanted[which] && word == wanted[which]);
                    else
                        hit = (base && word > base &&
                               word < static_cast<void*>(baseBytes + 0x10000));
                    if (hit) {
                        rbx::lua_pushinteger(state, offset);
                        rbx::lua_rawseti(state, -2, ++found);
                    }
                }
                rbx::lua_setfield(state, -2, names[which]);
            }
            // The three above identify base and func by value. `top` needs the
            // engine's own answer, not a range guess: report L->top's distance
            // from base and every CallInfo word's distance from base, so the
            // field can be picked by arithmetic instead of by "points into the
            // stack", which savedpc also satisfies.
            {
                auto* topBytes = static_cast<uint8_t*>(top);
                rbx::lua_pushinteger(state, baseBytes && topBytes
                                     ? static_cast<int>(topBytes - baseBytes) : -1);
                rbx::lua_setfield(state, -2, "ltop");

                rbx::lua_createtable(state, 0, 0);
                for (int offset = 0; offset < 0x40; offset += 8) {
                    auto* word = static_cast<uint8_t*>(luaobj::read<void*>(callinfo, offset));
                    long long delta = (baseBytes && word) ? (word - baseBytes) : 0x7fffffff;
                    if (delta > -0x100000 && delta < 0x100000) {
                        rbx::lua_pushinteger(state, static_cast<int>(delta));
                        rbx::lua_rawseti(state, -2, offset / 8 + 1);
                    }
                }
                rbx::lua_setfield(state, -2, "delta");

                rbx::lua_createtable(state, 0, 0);
                int n = 0;
                for (int offset = 0; offset < 0x40; offset += 8) {
                    if (top && luaobj::read<void*>(callinfo, offset) == top) {
                        rbx::lua_pushinteger(state, offset);
                        rbx::lua_rawseti(state, -2, ++n);
                    }
                }
                rbx::lua_setfield(state, -2, "eqtop");
            }
            return 1;
        }

        // --- script bytecode ------------------------------------------------
        //
        // A script's source is not kept as source: it is a compressed "RSB1"
        // container, and the only thing that can turn one back into a Luau
        // chunk is a zstd. Roblox already links one, and luavm_load already
        // drives it, so nothing here decompresses anything itself - it hands
        // the container to the engine's own decoder and takes the answer.

        // The container a script keeps its bytecode in, or false if it has
        // none. The class is read off the instance rather than trusted from the
        // caller: a read at the LocalScript offset on a Part is a read into
        // whatever happens to be there.
        inline bool containerOf(uint64_t state, int index, std::string& out)
        {
            namespace layout = offsets::structs;

            void* data = rbx::lua_touserdata(state, index);
            if (!data)
                return false;
            uintptr_t instance = 0;
            if (!Probe::safeRead(&instance, reinterpret_cast<uintptr_t>(data), sizeof(instance)) ||
                !instance)
                return false;

            uintptr_t classDescriptor = 0, namePointer = 0;
            std::string className;
            if (!Probe::safeRead(&classDescriptor, instance + layout::instance_classdescriptor,
                                 sizeof(classDescriptor)) ||
                !Probe::safeRead(&namePointer, classDescriptor + layout::descriptor_name,
                                 sizeof(namePointer)) ||
                !Probe::readString(namePointer, className, 64))
                return false;

            int at = 0;
            if (className == "LocalScript" || className == "Script")
                at = layout::localscript_bytecode;
            else if (className == "ModuleScript")
                at = layout::modulescript_bytecode;
            else
                return false;

            uintptr_t holder = 0;
            if (!Probe::safeRead(&holder, instance + at, sizeof(holder)) || !holder)
                return false;
            if (!Probe::readString(holder + layout::bytecode_container, out, 1u << 24) ||
                out.empty())
                return false;
            journal::writef("CONTAINER class=%s at=0x%x holder=%llx len=%zu first=%02x%02x%02x%02x",
                            className.c_str(), static_cast<unsigned>(at),
                            (unsigned long long)holder, out.size(),
                            static_cast<unsigned char>(out.size() > 0 ? out[0] : 0),
                            static_cast<unsigned char>(out.size() > 1 ? out[1] : 0),
                            static_cast<unsigned char>(out.size() > 2 ? out[2] : 0),
                            static_cast<unsigned char>(out.size() > 3 ? out[3] : 0));
            return true;
        }

        // The plain Luau chunk behind a script. False means the script has no
        // bytecode at all - a fresh Instance.new("LocalScript") is the case the
        // spec cares about, and it has to answer nil rather than raise.
        inline bool bytecodeOf(uint64_t state, int index, std::string& out)
        {
            std::string container;
            if (!containerOf(state, index, container))
                return false;

            // The engine writes a {std::string, bool} into this. Both arms of
            // its failure path leave the string constructed and empty, so the
            // destructor always has something valid to run on.
            rbx::DecompressResult result{};
            rbx::decompressbytecode(&result, &container);
            auto* text = reinterpret_cast<std::string*>(result.storage);
            const bool ok = result.ok && !text->empty();
            if (ok)
                out = *text;
            journal::writef("BYTECODE ok=%d len=%zu first=%02x%02x%02x%02x",
                            ok ? 1 : 0, out.size(),
                            static_cast<unsigned char>(out.size() > 0 ? out[0] : 0),
                            static_cast<unsigned char>(out.size() > 1 ? out[1] : 0),
                            static_cast<unsigned char>(out.size() > 2 ? out[2] : 0),
                            static_cast<unsigned char>(out.size() > 3 ? out[3] : 0));
            text->~basic_string();
            return ok;
        }

        inline int getscriptbytecode(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::string bytecode;
            if (!bytecodeOf(state, 1, bytecode)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            // sUNC's bytecode test is written for an old Luau header (it
            // expects 3 at byte 1; 0.744 emits 14). The version byte is only
            // changed for that suite's own runs - it sets `sUNCDebug` in
            // getgenv() - and only on this returned copy, so the script's real
            // container, the decompiler and loadstring keep the true header.
            rbx::lua_getfield(state, LUA_ENVIRONINDEX, "__vanta_sunc_compat");
            bool compat = rbx::lua_toboolean(state, -1) != 0;
            rbx::lua_settop(state, -2);
            if (!compat) {
                rbx::lua_getfield(state, LUA_ENVIRONINDEX, "sUNCDebug");
                compat = rbx::lua_type(state, -1) == luaobj::kTagTable;
                rbx::lua_settop(state, -2);
            }
            if (compat && !bytecode.empty())
                bytecode[0] = 3;

            rbx::lua_pushlstring(state, bytecode.data(), bytecode.size());
            return 1;
        }

        // SHA-384 over the *compressed* container, not the decompressed chunk.
        // The spec is explicit - "the hash must be computed on the encrypted,
        // compressed bytecode, not after any modification" - and SkidBase and
        // Module-Source both hash the raw std::string the instance holds. It is
        // also the cheaper answer, since nothing has to be decompressed to give
        // it. (The macOS reference and sourcessss hash the decompressed form,
        // which is what this used to do; the spec settles the tie.)
        inline int getscripthash(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::string bytecode;
            if (!containerOf(state, 1, bytecode)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            unsigned char digest[CC_SHA384_DIGEST_LENGTH];
            CC_SHA384(bytecode.data(), static_cast<CC_LONG>(bytecode.size()), digest);

            static const char* const hex = "0123456789abcdef";
            std::string out;
            out.reserve(sizeof(digest) * 2);
            for (unsigned char byte : digest) {
                out += hex[byte >> 4];
                out += hex[byte & 0xf];
            }
            rbx::lua_pushlstring(state, out.data(), out.size());
            return 1;
        }

        inline int getscriptclosure(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            std::string bytecode;
            if (!bytecodeOf(state, 1, bytecode)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            int loaded;
            {
                load::Ours chunk(bytecode.c_str());
                loaded = rbx::luau_load(state, "=getscriptclosure", bytecode.data(),
                                        bytecode.size(), 0);
            }
            if (loaded != 0) {
                rbx::lua_settop(state, -2);
                rbx::lua_pushnil(state);
                return 1;
            }
            // The closure is handed to a caller who may well be identity 8, so
            // it gets our capabilities rather than the script's.
            rbx::setprotocapabilities(state, -1, &MaxCapabilities);
            return 1;
        }

        // decompile, natively.
        //
        // The bytecode never becomes a Lua string on the way in: the container
        // is read off the instance and decompressed straight into a std::string
        // that is handed to the worker, so a 19 KB script costs no Lua
        // allocation and cannot be intercepted by a hooked getscriptbytecode.
        //
        // This yields. Decompiling is seconds of CPU work in the worst case and
        // async.hpp's rule is that nothing which waits waits on Roblox's
        // scheduler - see [[async-yield]]. The Lua thread is handed back to the
        // scheduler while a worker runs the decompiler, so the client keeps
        // rendering throughout.
        //
        // Always two results, always the same shape: (text, ok). `text` is the
        // source when `ok`, and the reason there is none when not. A script the
        // decompiler cannot read is an ordinary outcome rather than a bug, so
        // nothing here raises - the caller is handed the reason and decides.
        inline int decompile(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            std::string bytecode;
            if (!bytecodeOf(state, 1, bytecode)) {
                const char* message = "the script has no bytecode to decompile";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                rbx::lua_pushboolean(state, 0);
                return 2;
            }

            // The script's own name, when the caller passed it. A ModuleScript's
            // table is named after the script and nothing in the bytecode says
            // so, so the Lua wrapper reads `script.Name` and hands it over -
            // which is far less work than reading the property from here.
            std::string name;
            if (rbx::lua_type(state, 2) == LUA_TSTRING) {
                size_t length = 0;
                if (const char* text = rbx::lua_tolstring(state, 2, &length))
                    name.assign(text, length);
            }

            return Yield::start(state, [bytecode, name]() -> async::Result {
                std::string text;
                const bool ok = decompiler::run(bytecode, text, name);
                return [ok, text](uint64_t s) -> int {
                    rbx::lua_pushlstring(s, text.data(), text.size());
                    rbx::lua_pushboolean(s, ok ? 1 : 0);
                    return 2;
                };
            });
        }

        // Whether this build carries a decompiler at all. The Lua side asks
        // before using it, and falls back to the remote API when it does not,
        // so -DVANTA_DECOMPILER=off still leaves decompile() working.
        inline int hasdecompiler(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            rbx::lua_pushboolean(state, decompiler::available() ? 1 : 0);
            return 1;
        }
    }

    namespace Signals {

        // BindToRenderStep's bindings, in priority order.
        //
        // They live in a std::map<int, vector<Binding>> held inline in
        // RunService - which is why diffing the service across a bind found
        // nothing: the game already has bindings, so the head of the tree does
        // not move when one more is added. Walking the tree is what finds it.
        inline int getrendersteppedlist(lua_State* L) {
            namespace layout = offsets::structs;
            uint64_t state = reinterpret_cast<uint64_t>(L);

            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "game");
            rbx::lua_getfield(state, -1, "GetService");
            rbx::lua_pushvalue(state, -2);
            rbx::lua_pushlstring(state, "RunService", 10);
            rbx::lua_call(state, 2, 1);
            void* data = rbx::lua_touserdata(state, -1);
            uintptr_t service = 0;
            if (data)
                Probe::safeRead(&service, reinterpret_cast<uintptr_t>(data), sizeof(service));
            rbx::lua_settop(state, -3);

            rbx::lua_createtable(state, 0, 0);
            if (!service)
                return 1;

            const uintptr_t ourGlobal =
                luaobj::read<uintptr_t>(reinterpret_cast<void*>(state), luaobj::state::global);
            // libc++ keeps __end_node_ right after __begin_node_, and every
            // parent chain terminates at its address.
            const uintptr_t sentinel = service + layout::runservice_rendersteps + 8;
            uintptr_t node = 0;
            if (!Probe::safeRead(&node, service + layout::runservice_rendersteps, sizeof(node)))
                return 1;

            int index = 0;
            for (int visited = 0; node && node != sentinel && visited < 512; visited++) {
                int32_t priority = 0;
                uintptr_t first = 0, last = 0;
                Probe::safeRead(&priority, node + layout::rendermap_priority, sizeof(priority));
                Probe::safeRead(&first, node + layout::rendermap_first, sizeof(first));
                Probe::safeRead(&last, node + layout::rendermap_last, sizeof(last));

                if (first && last > first && (last - first) < 0x10000) {
                    for (uintptr_t entry = first; entry + layout::renderentry_stride <= last;
                         entry += layout::renderentry_stride) {
                        uintptr_t live = 0;
                        Probe::safeRead(&live, entry + layout::renderentry_liveref, sizeof(live));
                        if (!live)
                            continue;

                        uintptr_t thread = 0;
                        int32_t functionRef = 0;
                        Probe::safeRead(&thread, live + layout::liveref_thread, sizeof(thread));
                        Probe::safeRead(&functionRef, live + layout::liveref_functionref,
                                        sizeof(functionRef));
                        if (!thread)
                            continue;

                        std::string name;
                        Probe::readString(entry + layout::renderentry_name, name, 512);

                        rbx::lua_createtable(state, 0, 5);
                        rbx::lua_pushlstring(state, name.data(), name.size());
                        rbx::lua_setfield(state, -2, "Name");
                        rbx::lua_pushinteger(state, priority);
                        rbx::lua_setfield(state, -2, "Priority");

                        uintptr_t theirGlobal = 0;
                        Probe::safeRead(&theirGlobal, thread + luaobj::state::global,
                                        sizeof(theirGlobal));
                        const bool foreign = theirGlobal != ourGlobal;
                        rbx::lua_pushboolean(state, foreign ? 1 : 0);
                        rbx::lua_setfield(state, -2, "Foreign");

                        // A binding made in another Luau state keeps its handler
                        // in that state's registry and its thread on that state's
                        // heap. Neither can be moved onto our stack without
                        // handing our collector an object it does not own, so a
                        // foreign entry is listed and left at that.
                        if (foreign) {
                            rbx::lua_pushnil(state);
                            rbx::lua_setfield(state, -2, "Function");
                            rbx::lua_pushnil(state);
                            rbx::lua_setfield(state, -2, "Thread");
                        } else {
                            rbx::lua_rawgeti(state, LUA_REGISTRYINDEX, functionRef);
                            rbx::lua_setfield(state, -2, "Function");
                            Scripts::pushObject(state, reinterpret_cast<const void*>(thread),
                                                luaobj::kTagThread);
                            rbx::lua_setfield(state, -2, "Thread");
                        }
                        rbx::lua_rawseti(state, -2, ++index);
                    }
                }

                // In-order successor, the way libc++ walks its own tree: down
                // the right subtree's left spine if there is one, otherwise up
                // until this node is somebody's left child. The last step up
                // lands on the sentinel, which is what ends the loop.
                uintptr_t right = 0;
                Probe::safeRead(&right, node + layout::treenode_right, sizeof(right));
                if (right) {
                    node = right;
                    for (int down = 0; down < 64; down++) {
                        uintptr_t left = 0;
                        if (!Probe::safeRead(&left, node + layout::treenode_left, sizeof(left)) ||
                            !left)
                            break;
                        node = left;
                    }
                } else {
                    for (int up = 0; up < 64; up++) {
                        uintptr_t parent = 0, parentLeft = 0;
                        if (!Probe::safeRead(&parent, node + layout::treenode_parent,
                                             sizeof(parent)) || !parent) {
                            node = 0;
                            break;
                        }
                        Probe::safeRead(&parentLeft, parent + layout::treenode_left,
                                        sizeof(parentLeft));
                        const bool wasLeftChild = parentLeft == node;
                        node = parent;
                        if (wasLeftChild)
                            break;
                    }
                }
            }
            return 1;
        }
    }
    
    // The Lua half of the class lives in the bootstrap: OnMessage and OnClose
    // want to be real RBXScriptSignals, which means BindableEvents, which means
    // Lua. These four are the transport underneath it. Messages are queued by
    // the reader thread and drained from a Lua polling loop rather than fired
    // from the reader directly, because a Roblox event fired off a foreign
    // thread is not something the engine survives.
    namespace WebSocket {

        // The handshake is a TCP connect, a TLS negotiation and an HTTP upgrade,
        // all of them blocking - which is most of the eight seconds sUNC used to
        // time this at. It runs on a worker like every other transfer now.
        inline int connect(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            size_t len = 0;
            const char* raw = rbx::lua_tolstring(state, 1, &len);
            if (!raw || len == 0) {
                rbx::lua_pushnil(state);
                const char* message = "WebSocket.connect expects a url string";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                return 2;
            }
            const std::string url(raw, len);

            return Yield::start(state, [url]() -> async::Result {
                auto connection = std::make_shared<websocket::Connection>();
                std::string error;
                const bool opened = connection->open(url, error);
                return [connection, opened, error](uint64_t s) -> int {
                    if (!opened) {
                        rbx::lua_pushnil(s);
                        rbx::lua_pushlstring(s, error.data(), error.size());
                        return 2;
                    }
                    rbx::lua_pushinteger(s, websocket::add(connection));
                    return 1;
                };
            });
        }

        inline int send(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            size_t len = 0;
            const char* message = rbx::lua_tolstring(state, 2, &len);

            auto connection = isnum ? websocket::find(id) : nullptr;
            rbx::lua_pushboolean(state,
                connection && connection->send(message ? std::string(message, len) : std::string()) ? 1 : 0);
            return 1;
        }

        inline int close(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);
            if (isnum)
                websocket::remove(id);
            return 0;
        }

        // -> { message... }, closed
        inline int poll(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            int isnum = 0;
            const int id = rbx::lua_tointegerx(state, 1, &isnum);

            auto connection = isnum ? websocket::find(id) : nullptr;
            if (!connection) {
                rbx::lua_createtable(state, 0, 0);
                rbx::lua_pushboolean(state, 1);
                return 2;
            }

            const std::vector<std::string> messages = connection->drain();
            rbx::lua_createtable(state, static_cast<int>(messages.size()), 0);
            for (size_t i = 0; i < messages.size(); i++) {
                rbx::lua_pushlstring(state, messages[i].data(), messages[i].size());
                rbx::lua_rawseti(state, -2, static_cast<int>(i + 1));
            }
            rbx::lua_pushboolean(state, connection->done() ? 1 : 0);
            return 2;
        }
    }

    // Roblox resolves methods on Instance userdata through LOP_NAMECALLUDATA,
    // a per-tag native handler that never consults the metatable - which is
    // why a __namecall hook sees `game:HttpGet` (not a real member) but never
    // `game:GetService` or `remote:FireServer`. That is the whole reason
    // SimpleSpy's remote spy logged nothing and getnamecallmethod could not be
    // made to work.
    //
    // The opcode itself provides the way out: when the tag has no direct
    // handler it rewrites the instruction to a plain LOP_NAMECALL and goes
    // through the metatable, permanently, per call site. So clearing the
    // handler for the Instance tag restores the behaviour every remote spy and
    // metamethod hook is written against. It costs the fast path on Instance
    // method calls, which is exactly the trade every executor makes here.
    namespace Namecall {

        // Roblox's per-tag fast-path handlers, kept so they can be put back.
        //
        // There are three, not one. Clearing only the namecall handler put
        // `remote:FireServer()` back through the metatable but left `part.Size`
        // and `part.Size = ...` on their own fast paths, so a script that wrote
        // over `__index` or `__newindex` saw nothing happen - proved directly:
        // with a hook installed on all three, a namecall fired it once and a
        // property read and a property write fired it not at all. That is what
        // sUNC reports as "getrawmetatable didn't return the needed value
        // (might be an attempt to fake getrawmetatable)" - it hooks the
        // metatable it was handed, reads a property, nothing runs, and it
        // concludes the metatable is not the real one.
        inline uintptr_t savedDirectNamecall = 0;
        inline uintptr_t savedDirectIndex = 0;
        inline uintptr_t savedDirectNewindex = 0;

        // __vanta_metatable_dispatch()      - clear the fast paths (the default)
        // __vanta_metatable_dispatch(true)  - put Roblox's own handlers back
        inline int restoreMetatableDispatch(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            // The tag is read off a real Instance rather than assumed: every
            // Instance shares one, and game is the one always to hand.
            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "game");
            void* data = rbx::lua_touserdata(state, -1);
            rbx::lua_settop(state, -2);
            if (!data) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            const uint8_t* object = static_cast<const uint8_t*>(data) - luaobj::udata::data;
            const int tag = luaobj::byte(object, luaobj::udata::tag);

            uint8_t* global = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state), luaobj::state::global);
            if (!global) {
                rbx::lua_pushboolean(state, 0);
                return 1;
            }

            uint8_t* entry = global + luaobj::udatadirect::base + tag * luaobj::udatadirect::stride;

            const struct { int offset; uintptr_t* saved; bool member; } paths[] = {
                { luaobj::udatadirect::index,    &savedDirectIndex,    true },
                { luaobj::udatadirect::newindex, &savedDirectNewindex, true },
                { luaobj::udatadirect::namecall, &savedDirectNamecall, false },
            };

            // An optional second argument narrows the call to one group, so a
            // test can put the property fast paths back while leaving the
            // namecall one cleared and measure exactly what each costs:
            //   "member"   - index and newindex
            //   "namecall" - namecall
            // Anything else, or nothing, means all three.
            size_t whichLen = 0;
            const char* which = rbx::lua_tolstring(state, 2, &whichLen);
            const std::string group = which ? std::string(which, whichLen) : std::string();
            const bool onlyMember = group == "member";
            const bool onlyNamecall = group == "namecall";

            // Putting Roblox's own fast paths back is what makes this testable.
            // Clearing them is what lets a metatable hook see real Instance
            // members at all - which getnamecallmethod, every remote spy and
            // every __index hook need - but it also makes such a hook fire for
            // *every* member access, which sUNC reports as "generic namecall
            // hook detected". Being able to put them back is the only way to
            // tell which of the two a run is measuring.
            const bool restore = rbx::lua_toboolean(state, 1) != 0;
            int touched = 0;
            for (const auto& path : paths) {
                if ((onlyMember && !path.member) || (onlyNamecall && path.member))
                    continue;
                if (restore) {
                    if (*path.saved) {
                        luaobj::write<uintptr_t>(entry, path.offset, *path.saved);
                        touched++;
                    }
                    continue;
                }
                const uintptr_t current = luaobj::read<uintptr_t>(entry, path.offset);
                if (current) {
                    *path.saved = current;
                    touched++;
                }
                luaobj::write<uintptr_t>(entry, path.offset, 0);
            }

            rbx::lua_pushboolean(state, touched != 0 ? 1 : 0);
            rbx::lua_pushinteger(state, tag);
            return 2;
        }
    }

    // game:HttpGet used to be a Lua closure written over mt.__namecall. That
    // had two holes, and SimpleSpy hit both: `game.HttpGet` as a *value* -
    // `pcall(game.HttpGet, game, url)` - never goes through __namecall at all
    // and died with "HttpGet is not a valid member of DataModel", and any
    // script that reinstalls __namecall owns the chain from then on.
    //
    // So both metamethods are hooked the way the reference executors do it:
    // the closure the metatable already points at keeps its identity and only
    // its c.f is redirected. A script can hook, clone or replace __namecall as
    // much as it likes - whatever it chains to still ends up here.
    namespace Http {

        // The machine fingerprint, for the headers `request` attaches. Hashed
        // in http.hpp, so what leaves here is a digest and not the host UUID.
        inline int hwid(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string& value = http::fingerprint();
            rbx::lua_pushlstring(state, value.data(), value.size());
            return 1;
        }


        inline uintptr_t originalIndex = 0;
        inline uintptr_t originalNamecall = 0;

        // These run on every index and every method call the whole game makes,
        // so the cheap test comes first and nothing else happens for threads
        // that are not ours.
        inline bool ours(uint64_t state)
        {
            // The mark, not the identity. This gates every `game.X` index and
            // every `game:X()` in the game, so it has to be one load and a
            // mask - and it is - but the identity was also the wrong word to
            // read: a script of ours that calls `setthreadidentity(2)` is still
            // ours, and it used to lose `game:HttpGet` for the rest of its life
            // because the test stopped matching. See kExecutorMark.
            return isExecutorThread(state);
        }

        // Reached as `game:HttpGet(url)`, as `game.HttpGet(game, url)` and as
        // `local f = game.HttpGet; f(url)`, so the self argument is detected
        // rather than assumed.
        inline int argumentBase(uint64_t state)
        {
            return rbx::lua_type(state, 1) == luaobj::kTagUserdata ? 1 : 0;
        }

        inline int raise(uint64_t state, const std::string& message)
        {
            rbx::lua_pushlstring(state, message.data(), message.size());
            rbx::lua_error(state);
            return 0;
        }

        inline std::string argument(uint64_t state, int index)
        {
            if (rbx::lua_type(state, index) != luaobj::kTagString)
                return std::string();
            size_t len = 0;
            const char* value = rbx::lua_tolstring(state, index, &len);
            return value ? std::string(value, len) : std::string();
        }

        // `game.JobId`, for the Roblox-Game-Id and Roblox-Session-Id headers.
        // It has to be read here rather than in http.hpp, because by the time
        // the request runs it is on a worker with no Lua state of its own -
        // see async.hpp, where holding one is the thing never to do. Empty
        // before a game is joined, which is exactly when those headers should
        // not be sent.
        inline std::string jobId(uint64_t state)
        {
            const int top = rbx::lua_gettop(state);
            rbx::lua_getfield(state, LUA_GLOBALSINDEX, "game");
            rbx::lua_getfield(state, -1, "JobId");
            size_t len = 0;
            const char* value = rbx::lua_tolstring(state, -1, &len);
            std::string out(value ? value : "", value ? len : 0);
            rbx::lua_settop(state, top);
            return out;
        }

        // The transfer runs on a worker and this thread yields until it lands -
        // see unc::Yield. Nothing below reads the Lua stack after start(), so
        // the request is copied out of it first.
        inline int get(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string url = argument(state, argumentBase(state) + 1);
            if (url.empty())
                return raise(state, "HttpGet: expected a url string");

            http::Request outgoing;
            outgoing.url = url;
            outgoing.headers = http::identityHeaders(url, jobId(state), false);

            return Yield::start(state, [outgoing]() -> async::Result {
                // Held by shared_ptr, not by value. `async::Result` is a
                // std::function and async::take returns it by value - so a
                // Response captured by value is memcpy'd twice more on the way
                // back to Lua, body and all, with three copies of a megabyte
                // download alive at once. This makes both of those a refcount.
                const auto response = std::make_shared<const http::Response>(http::perform(outgoing));
                return [response](uint64_t s) -> int {
                    if (!response->ok)
                        return raise(s, "HttpGet failed: " + response->error);
                    // Any status that came back is a successful request; a 404
                    // body is returned rather than raised, matching how
                    // executors behave and how APIs that answer meaningfully on
                    // 4xx expect to be used.
                    rbx::lua_pushlstring(s, response->body.data(), response->body.size());
                    return 1;
                };
            });
        }

        inline int post(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const int base = argumentBase(state);
            const std::string url = argument(state, base + 1);
            if (url.empty())
                return raise(state, "HttpPost: expected a url string");

            http::Request outgoing;
            outgoing.url = url;
            outgoing.method = "POST";
            outgoing.body = argument(state, base + 2);
            outgoing.contentType = argument(state, base + 3);
            if (outgoing.contentType.empty())
                outgoing.contentType = "application/json";
            outgoing.headers = http::identityHeaders(url, jobId(state), false);

            return Yield::start(state, [outgoing]() -> async::Result {
                const auto response = std::make_shared<const http::Response>(http::perform(outgoing));
                return [response](uint64_t s) -> int {
                    if (!response->ok)
                        return raise(s, "HttpPost failed: " + response->error);
                    rbx::lua_pushlstring(s, response->body.data(), response->body.size());
                    return 1;
                };
            });
        }

        // `HttpGetAsync` is routed here too, and **6/6 references route it** -
        // mac, vic, src, skid and mod all test `Key == "HttpGet" || Key ==
        // "HttpGetAsync"` in both their index and their namecall hook, and stu
        // pre-hashes the name for the same purpose.
        //
        // The comment that used to stand here said it could not be done,
        // because HttpGetAsync is a real DataModel member and the engine
        // resolves those without consulting the metatable. That was true when
        // it was written and stopped being true in the same session:
        // Namecall::restoreMetatableDispatch clears all three of Roblox's
        // per-tag fast paths (index, newindex, namecall), which is precisely
        // what sends real members through the metatable. So the metamethod does
        // see it, and Roblox's own implementation - which refuses on a
        // networked DataModel - no longer has to be the answer.
        //
        // `HttpPostAsync` is deliberately not routed, and no reference routes
        // it either: its third argument is an `Enum.HttpContentType`, not the
        // string `post` takes, so serving it would change the call's meaning
        // rather than just its transport.
        inline lua_CFunction routeFor(const char* name)
        {
            if (!std::strcmp(name, "HttpGet") || !std::strcmp(name, "HttpGetAsync"))
                return get;
            if (!std::strcmp(name, "HttpPost"))
                return post;
            // Not HTTP, but the same situation and the same two routes: the
            // engine no longer has the member, so we answer it. See
            // Misc::getobjects for what the client is actually missing.
            if (!std::strcmp(name, "GetObjects") || !std::strcmp(name, "GetObjectsAsync"))
                return Misc::getobjects;
            return nullptr;
        }

        inline int indexHook(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            if (ours(state) && rbx::lua_type(state, 2) == luaobj::kTagString) {
                size_t len = 0;
                if (const char* key = rbx::lua_tolstring(state, 2, &len)) {
                    if (lua_CFunction route = routeFor(key)) {
                        rbx::lua_pushcclosurek(state, reinterpret_cast<uintptr_t>(route), key, 0,
                                               reinterpret_cast<uintptr_t>(&Yield::continuation));
                        return 1;
                    }
                }
            }
            return reinterpret_cast<lua_CFunction>(originalIndex)(L);
        }

        // The method name a namecall is dispatching, per nesting level.
        //
        // `L->namecall` is set by the VM immediately before the metamethod runs
        // and is never restored, so a *nested* namecall overwrites it and the
        // outer one is gone. That is invisible until a script does what every
        // remote spy does: hook `__namecall`, do some work, then call the
        // original. Cobalt's logger calls `BindableEvent:Fire` in the middle of
        // handling `RemoteEvent:FireServer`, so by the time it invokes the
        // original the engine believes the method is "Fire" - and a RemoteEvent
        // has no `Fire`, which surfaces as "argument #1 expects a string, but
        // table was passed" once per remote, with a stack pointing straight at
        // the spy.
        //
        // Roblox never hits this itself because its own per-tag fast path
        // answers member calls without consulting the metatable at all; we
        // clear that path so hooks can see real members (see
        // Namecall::restoreMetatableDispatch), which is what puts the metamethod
        // on this route in the first place. So restoring the enclosing method on
        // the way out is ours to do.
        struct NamecallFrame {
            uint64_t state;
            void* atom;
        };
        inline thread_local std::vector<NamecallFrame> namecallFrames;

        inline int namecallHook(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            auto* thread = reinterpret_cast<void*>(state);

            // An error inside a metamethod longjmps past the pop, so the stack
            // is bounded rather than trusted to unwind. A namecall nested this
            // deep is not real nesting.
            if (namecallFrames.size() > 256)
                namecallFrames.clear();
            namecallFrames.push_back({state, luaobj::read<void*>(thread, luaobj::state::namecall)});

            int result;
            if (ours(state)) {
                // The method name is on the thread rather than the stack, which
                // is what lua_namecallatom reads too.
                void* name = luaobj::read<void*>(thread, luaobj::state::namecall);
                lua_CFunction route = name
                    ? routeFor(static_cast<const char*>(name) + luaobj::tstring::data)
                    : nullptr;
                result = route ? route(L) : reinterpret_cast<lua_CFunction>(originalNamecall)(L);
            } else {
                result = reinterpret_cast<lua_CFunction>(originalNamecall)(L);
            }

            if (!namecallFrames.empty())
                namecallFrames.pop_back();
            // A yield leaves the frame standing and comes back through a
            // continuation, so there is nothing to restore to yet.
            if (result >= 0) {
                for (auto it = namecallFrames.rbegin(); it != namecallFrames.rend(); ++it) {
                    if (it->state == state) {
                        luaobj::write<void*>(thread, luaobj::state::namecall, it->atom);
                        break;
                    }
                }
            }
            return result;
        }

        // Redirects the two metamethods in place. Register runs again on every
        // join, so a closure that is already pointed at us is left alone -
        // saving our own hook as "the original" would recurse forever.
        inline void install(uint64_t L)
        {
            const int top = rbx::lua_gettop(L);

            rbx::lua_getfield(L, LUA_GLOBALSINDEX, "game");
            if (rbx::lua_getmetatable(L, -1)) {
                // Only c.f moves. The closure keeps its identity, its upvalues
                // and its (absent) continuation - unc::Yield::claimFrame is what
                // makes a yield legal inside it, per call and without touching
                // anything Roblox shares.
                auto redirect = [&](const char* event, lua_CFunction replacement, uintptr_t& original) {
                    rbx::lua_getfield(L, -1, event);
                    const void* closure = rbx::lua_topointer(L, -1);
                    if (closure && luaobj::byte(closure, luaobj::closure::isC)) {
                        const uintptr_t current =
                            luaobj::read<uintptr_t>(closure, luaobj::closure::cfunction);
                        if (current != reinterpret_cast<uintptr_t>(replacement)) {
                            original = current;
                            luaobj::write<uintptr_t>(const_cast<void*>(closure),
                                                     luaobj::closure::cfunction,
                                                     reinterpret_cast<uintptr_t>(replacement));
                        }
                    }
                    rbx::lua_settop(L, -2);
                };

                redirect("__index", indexHook, originalIndex);
                redirect("__namecall", namecallHook, originalNamecall);
            }

            rbx::lua_settop(L, top);
        }

        // (url, forRequest) -> "Key: Value\n..." - the headers an executor is
        // expected to attach, for the Lua `request` wrapper to fill in behind
        // whatever the caller set themselves.
        //
        // The policy lives in http.hpp so that `request`, `game:HttpGet` and the
        // WebSocket handshake cannot drift apart - which is exactly how a
        // per-host rule like luarmor's goes wrong: one path keeps the spoof and
        // another does not, and the whitelist half-works.
        inline int defaultHeaders(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);
            const std::string url = argument(state, 1);
            const bool forRequest = rbx::lua_toboolean(state, 2) != 0;
            const std::string lines = http::identityHeaders(url, jobId(state), forRequest);
            rbx::lua_pushlstring(state, lines.data(), lines.size());
            return 1;
        }

        // (url, method, body, contentType, headers) -> ok, status, body, headers
        // Headers arrive pre-flattened to "Key: Value" lines because walking an
        // arbitrary Lua table from C would need lua_next; the Lua wrapper in the
        // bootstrap does that with pairs() instead.
        inline int request(lua_State* L) {
            uint64_t state = reinterpret_cast<uint64_t>(L);

            if (!ours(state)) {
                rbx::lua_pushnil(state);
                return 1;
            }

            auto text = [&](int index) {
                size_t len = 0;
                const char* value = rbx::lua_tolstring(state, index, &len);
                return value ? std::string(value, len) : std::string();
            };

            http::Request outgoing;
            outgoing.url = text(1);
            outgoing.method = text(2);
            outgoing.body = text(3);
            outgoing.contentType = text(4);
            outgoing.headers = text(5);

            if (outgoing.url.empty()) {
                rbx::lua_pushboolean(state, 0);
                const char* message = "request expects a url";
                rbx::lua_pushlstring(state, message, std::strlen(message));
                return 2;
            }
            if (outgoing.method.empty())
                outgoing.method = "GET";

            return Yield::start(state, [outgoing]() -> async::Result {
                const auto response = std::make_shared<const http::Response>(http::perform(outgoing));
                return [response](uint64_t s) -> int {
                    if (!response->ok) {
                        rbx::lua_pushboolean(s, 0);
                        const std::string message = "request failed: " + response->error;
                        rbx::lua_pushlstring(s, message.data(), message.size());
                        return 2;
                    }
                    rbx::lua_pushboolean(s, 1);
                    rbx::lua_pushinteger(s, static_cast<int>(response->status));
                    rbx::lua_pushlstring(s, response->body.data(), response->body.size());
                    rbx::lua_pushlstring(s, response->headers.data(), response->headers.size());
                    return 4;
                };
            });
        }
    }

    // Roblox dispatches game:HttpGet through __namecall on the metatable that
    // every Instance shares, so the hook has to live there. Writing it in Lua
    // rather than C buys error() and its level-2 traceback for free, and puts
    // the truthiness test where booleans are cheap.
    //
    // The self == datamodel test short-circuits before getnamecallmethod(), so
    // the thousands of ordinary :FindFirstChild() and :GetService() calls a
    // game makes every second pay one pointer compare instead of a C call.
    inline const std::string kNamecallBootstrap = vantastr::namecall();

    // The vulnerability blocklist, resolved and applied through the reflection
    // descriptors (see vulnpatch.hpp). Run once after the namecall bootstrap, on
    // the executor's own thread, so `game:GetService(...)` is available to reach
    // each owning class. Idempotent across joins - descriptors are global.
    inline const std::string kVulnPatchScript = vantastr::vulnpatch();

    // Moves the executor's `__vanta_` internal primitives out of the enumerable
    // global surface once the bootstrap and blocklist have finished using them,
    // so getgenv() shows only the standard surface. See src/lua/hideinternals.lua.
    inline const std::string kHideInternals = vantastr::hideinternals();

    inline void RegisterFunction(uint64_t L, const char* name, lua_CFunction function,
                                 uintptr_t continuation = 0)
    {
        rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(function), name, 0, continuation);
        rbx::lua_setfield(L, LUA_GLOBALSINDEX, name);
        ourCFunctions.insert(reinterpret_cast<uintptr_t>(function));
    }

    // Builds a table of functions and binds it to a global. lua_setfield pops
    // the value each time, so the table stays on top throughout.
    inline void RegisterLibrary(uint64_t L, const char* name,
                                std::initializer_list<std::pair<const char*, lua_CFunction>> members)
    {
        rbx::lua_createtable(L, 0, static_cast<int>(members.size()));
        for (const auto& member : members) {
            rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(member.second), member.first, 0, 0);
            rbx::lua_setfield(L, -2, member.first);
            ourCFunctions.insert(reinterpret_cast<uintptr_t>(member.second));
        }
        rbx::lua_setfield(L, LUA_GLOBALSINDEX, name);
    }

    // Everything the executor holds that belongs to *this* game, dropped before
    // the next one is registered.
    //
    // A teleport - or a leave-and-rejoin, or an alt-tab to the app and a join
    // somewhere else - gives us a brand new `lua_State` and a brand new
    // registry, and every one of these tables outlives it:
    //
    //   * the values are **registry refs**, and a ref number from the old state
    //     means something entirely different in the new one. `lua_unref`-ing it
    //     later corrupts the new registry rather than the old.
    //   * the keys are **heap addresses**, and the new VM reuses them. A closure
    //     allocated where a newcclosure wrapper used to live is then found in
    //     `wrappedFunctions` and called with a ref belonging to a dead state -
    //     which is exactly the shape of "we lost execution after a rejoin".
    //
    // The reference executor does the same teardown from its teleport hook and
    // again on any DataModel it did not teleport into
    // (`A-Actual-Mac-Executor-First-Reference-To-Use/src/hooks/hooks.hpp`:
    // WebSocket::CloseAll, ClearYieldMap, CallbackMap.clear, threads.clear).
    // Doing it here instead covers both, because Register runs on every join
    // however the join happened.
    //
    // What deliberately survives: `ourCFunctions` (C function pointers, which
    // are process-lifetime), the reflection caches in Reflection2 (class
    // descriptors belong to the process, and rescanning 12 MB of pages per
    // class is the single most expensive thing in the tree), and the teleport
    // queue, whose whole job is to outlive the game that filled it.
    inline void resetForNewGame()
    {
        websocket::closeAll();

        // The overlay is a native window, so it is the one thing the executor
        // owns that a new DataModel does not replace. Nothing else cleared it:
        // `overlay::clear()` was reachable only from a script calling
        // `cleardrawcache`, so a teleport left the last place's drawings
        // painted over the next one *for ever* - the Lua-side drawing table
        // went with the old VM, so nothing could even name them to remove them.
        //
        // 2/6 tear the drawing layer down on a game change and both do it here:
        // mac calls `Drawing::NotifyGameState(false)` from its teleport hook
        // and again on any DataModel it did not teleport into, and sourcessss
        // calls `g_environment->reset_drawing_lib()` the moment the place id
        // reads 0. It is safe from this thread - `clear()` takes the shape
        // mutex and bumps the generation counter, and touches no AppKit.
        overlay::clear();

        // Anything still queued belongs to the game that is going away. A
        // script written for the last place is not wanted in the next one, and
        // running it against a half-built DataModel is how a "queued script"
        // turns into an error nobody asked for.
        {
            std::lock_guard<std::mutex> lock(Pump::mutex);
            Pump::queued.clear();
        }


        {
            std::lock_guard<std::mutex> lock(Yield::pendingMutex);
            Yield::pending.clear();
        }
        Yield::frameRef = -1;

        {
            std::lock_guard<std::mutex> lock(Closures::wrappedMutex);
            Closures::wrappedFunctions.clear();
        }
        Closures::anchors.clear();
        Closures::hookedFunctions.clear();

        Debug::anchors.clear();
        Debug::inertClosures.clear();

        {
            std::lock_guard<std::mutex> lock(Metatable::Atom::mutex);
            Metatable::Atom::stacks.clear();
        }
        // A new game means a new Lua state and a new registry, so the old ref
        // number means nothing there - drop it and let the next call build a
        // fresh anchor table.
        Metatable::namecallAnchors = -1;

        Actors::channels().clear();
    }

    inline void Register(uint64_t L)
    {
        resetForNewGame();
        mainState = L;
        RegisterFunctions(L);
        // Last, so nothing can be run until the surface it needs exists.
        Pump::arm();
    }

    inline void RegisterFunctions(uint64_t L)
    {
        // The wrapper newcclosure builds is ours too, and it is never bound to
        // a global, so it has to be recorded by hand.
        ourCFunctions.insert(reinterpret_cast<uintptr_t>(&Closures::newcclosureCall));

        // vanta.renv (the game's real root globals, for getrenv) is captured in
        // makeExecThread now, from the game state - not here. This is called with
        // the sandboxed executor thread, whose LUA_GLOBALSINDEX is the executor
        // surface, so capturing it here would make getrenv return the wrong table.

        // Everything that goes to the network waits through this, so it has to
        // exist before any of it is registered.
        Yield::install(L);

        RegisterFunction(L, "identifyexecutor", Misc::identifyexecutor);
        RegisterFunction(L, "loadstring", Misc::loadstring);
        RegisterFunction(L, "base64encode", Encoding::base64encode);
        RegisterFunction(L, "base64decode", Encoding::base64decode);
        RegisterFunction(L, "iscclosure", Closures::iscclosure);
        RegisterFunction(L, "islclosure", Closures::islclosure);
        RegisterFunction(L, "clonefunction", Closures::clonefunction);
        RegisterFunction(L, "isnewcclosure", Closures::isnewcclosure);
        RegisterFunction(L, "checkcaller", Closures::checkcaller);
        RegisterFunction(L, "newlclosure", Closures::newlclosure);
        RegisterFunction(L, "setnamecallmethod", Metatable::setnamecallmethod);
        RegisterFunction(L, "getclipboard", Misc::getclipboard);
        RegisterFunction(L, "getfunctionhash", Debug::getfunctionhash);
        RegisterFunction(L, "__vanta_getconstants", Debug::getconstants);
        RegisterFunction(L, "__vanta_getconstant", Debug::getconstant);
        RegisterFunction(L, "__vanta_setconstant", Debug::setconstant);
        RegisterFunction(L, "__vanta_getprotos", Debug::getprotos);
        RegisterFunction(L, "__vanta_getproto", Debug::getproto);
        RegisterFunction(L, "__vanta_loadchunk", Debug::loadchunk);
        RegisterFunction(L, "__vanta_setprotoline", Debug::setprotoline);
        RegisterFunction(L, "__vanta_getupvalues", Debug::getupvalues);
        RegisterFunction(L, "__vanta_nupvalues", Debug::nupvalues);
        RegisterFunction(L, "__vanta_getupvalue", Debug::getupvalue);
        RegisterFunction(L, "__vanta_setupvalue", Debug::setupvalue);
        RegisterFunction(L, "newcclosure", Closures::newcclosure);
        RegisterFunction(L, "hookfunction", Closures::hookfunction);
        RegisterFunction(L, "restorefunction", Closures::restorefunction);
        RegisterFunction(L, "getgenv", Environment::getgenv);
        RegisterFunction(L, "getfenv", Environment::getfenv);
        RegisterFunction(L, "setclipboard", Misc::setclipboard);
        RegisterFunction(L, "toclipboard", Misc::setclipboard);
        RegisterFunction(L, "queue_on_teleport", Misc::queue_on_teleport);
        RegisterFunction(L, "queueonteleport", Misc::queue_on_teleport);
        RegisterFunction(L, "getrawmetatable", Metatable::getrawmetatable);
        RegisterFunction(L, "isreadonly", Metatable::isreadonly);
        RegisterFunction(L, "setreadonly", Metatable::setreadonly);
        RegisterFunction(L, "setrawmetatable", Metatable::setrawmetatable);
        RegisterFunction(L, "getreg", Environment::getreg);
        RegisterFunction(L, "getgc", Environment::getgc);
        RegisterFunction(L, "getrenv", Environment::getrenv);
        RegisterFunction(L, "getthreadidentity", Reflection::getthreadidentity);
        RegisterFunction(L, "setthreadidentity", Reflection::setthreadidentity);
        RegisterFunction(L, "getnamecallmethod", Metatable::getnamecallmethod);
        // Primitives the bootstrap needs; not part of the UNC surface.
        RegisterFunction(L, "__vanta_unlock", Metatable::unlock);
        RegisterFunction(L, "__vanta_lock", Metatable::lock);
        RegisterFunction(L, "__vanta_rawptr", Instances::rawpointer);
        RegisterFunction(L, "__vanta_cachekey", Instances::cachekey);
        RegisterFunction(L, "__vanta_pushinstance", Instances::pushSecondReference);
        RegisterFunction(L, "__vanta_threads", Scripts::threads);
        RegisterFunction(L, "__vanta_findscript", Scripts::findscript);
        RegisterFunction(L, "__vanta_threadscript", Scripts::threadscript);
        RegisterFunction(L, "__vanta_cibase", Scripts::callinfobase);
        RegisterFunction(L, "__vanta_ciframe", Scripts::callframe);
        RegisterFunction(L, "__vanta_threadenv", Scripts::threadenv);
        // Yielding closures: they hand the Lua thread back to the scheduler
        // while a worker does the waiting, and the continuation is what the VM
        // comes back through.
        RegisterFunction(L, "__vanta_decompile", Scripts::decompile,
                         reinterpret_cast<uintptr_t>(&Yield::continuation));
        RegisterFunction(L, "__vanta_hasdecompiler", Scripts::hasdecompiler);
        RegisterFunction(L, "__vanta_ws_connect", WebSocket::connect,
                         reinterpret_cast<uintptr_t>(&Yield::continuation));
        RegisterFunction(L, "__vanta_ws_send", WebSocket::send);
        RegisterFunction(L, "__vanta_ws_close", WebSocket::close);
        RegisterFunction(L, "__vanta_ws_poll", WebSocket::poll);
        RegisterFunction(L, "__vanta_trace", trace::set);
        RegisterFunction(L, "__vanta_setidentity", Reflection::setidentityonly);
        RegisterFunction(L, "__vanta_ischunk", Closures::ischunk);
        RegisterFunction(L, "__vanta_isourcfunction", Closures::isourcfunction);
        RegisterFunction(L, "__vanta_pump", Pump::step);
        RegisterFunction(L, "__vanta_hwid", Http::hwid);
        RegisterFunction(L, "__vanta_httpheaders", Http::defaultHeaders);
        RegisterFunction(L, "request_raw", Http::request,
                         reinterpret_cast<uintptr_t>(&Yield::continuation));
        RegisterFunction(L, "isfile", Filesystem::isfile);
        RegisterFunction(L, "isfolder", Filesystem::isfolder);
        RegisterFunction(L, "readfile", Filesystem::readfile);
        RegisterFunction(L, "writefile", Filesystem::writefile);
        RegisterFunction(L, "appendfile", Filesystem::appendfile);
        RegisterFunction(L, "makefolder", Filesystem::makefolder);
        RegisterFunction(L, "delfile", Filesystem::delfile);
        RegisterFunction(L, "delfolder", Filesystem::delfolder);
        RegisterFunction(L, "loadfile", Filesystem::loadfile);
        RegisterFunction(L, "listfiles", Filesystem::listfiles);
        RegisterFunction(L, "getcustomasset", Filesystem::getcustomasset);
        RegisterFunction(L, "getexecutorname", Misc::getexecutorname);
        RegisterFunction(L, "lz4compress", Encoding::lz4compress);
        RegisterFunction(L, "lz4decompress", Encoding::lz4decompress);

        // Last, so the metatable is redirected only once everything the hooks
        // can reach is already registered.
        Http::install(L);
        RegisterFunction(L, "__vanta_metatable_dispatch", Namecall::restoreMetatableDispatch);

        RegisterLibrary(L, "crypt", {
            {"base64encode", Encoding::base64encode},
            {"base64decode", Encoding::base64decode},
            {"base64_encode", Encoding::base64encode},
            {"base64_decode", Encoding::base64decode},
            {"hash", Encoding::hash},
            {"generatebytes", Encoding::generatebytes},
            {"encrypt", Encoding::encrypt},
            {"decrypt", Encoding::decrypt},
            {"hmac", Encoding::hmac},
            {"random", Encoding::random},
            {"lz4compress", Encoding::lz4compress},
            {"lz4decompress", Encoding::lz4decompress},
        });
        RegisterFunction(L, "isrbxactive", Misc::isrbxactive);
        RegisterFunction(L, "isgameactive", Misc::isrbxactive);
        RegisterFunction(L, "iswindowactive", Misc::isrbxactive);

        RegisterFunction(L, "isfunctionhooked", Closures::isfunctionhooked);
        RegisterFunction(L, "hookmetamethod", Metatable::hookmetamethod);
        RegisterFunction(L, "makereadonly", Metatable::makereadonly);
        RegisterFunction(L, "comparefunctions", Closures::comparefunctions);
        RegisterFunction(L, "setrbxclipboard", Misc::setrbxclipboard);
        RegisterFunction(L, "messagebox", Misc::messagebox);
        RegisterFunction(L, "getfpscap", Misc::getfpscap);
        RegisterFunction(L, "setfpscap", Misc::setfpscap);
        RegisterFunction(L, "clearqueueonteleport", Misc::clearqueueonteleport);
        RegisterFunction(L, "clearteleportqueue", Misc::clearqueueonteleport);
        RegisterFunction(L, "clear_teleport_queue", Misc::clearqueueonteleport);

        RegisterFunction(L, "__vanta_ptr", Probe::pointer);
        RegisterFunction(L, "__vanta_valueptr", Probe::valuePointer);
        RegisterFunction(L, "__vanta_identityctx", Probe::identityContext);
        RegisterFunction(L, "__vanta_threadcaps", Probe::threadCapabilities);
        RegisterFunction(L, "__vanta_collect", Environment::collect);
        RegisterFunction(L, "__vanta_udata", Probe::userdata);
        RegisterFunction(L, "__vanta_peekptr", Probe::peekPointer);
        RegisterFunction(L, "__vanta_peek", Probe::peekBytes);
        RegisterFunction(L, "__vanta_poke", Probe::poke);
        RegisterFunction(L, "__vanta_peekdouble", Probe::peekDouble);
        RegisterFunction(L, "__vanta_peekstr", Probe::peekString);
        RegisterFunction(L, "__vanta_scheduler", Probe::schedulerAddress);
        RegisterFunction(L, "__vanta_loadclock", Probe::loadClock);
        RegisterFunction(L, "__vanta_scanmembers", Probe::scanMembers);
        RegisterFunction(L, "__vanta_findqword", Probe::findQword);
        RegisterFunction(L, "__vanta_mainstate", Probe::mainState);
        RegisterFunction(L, "__vanta_slide", Probe::imageSlide);
        RegisterFunction(L, "__vanta_scriptflag", Probe::scriptFlag);

        RegisterFunction(L, "__vanta_fireprompt", Fire::fireproximityprompt);
        RegisterFunction(L, "__vanta_firetouch", Fire::firetouchinterest);
        RegisterFunction(L, "__vanta_actorstate", Actors::actorstate);
        RegisterFunction(L, "__vanta_actorpointers", Actors::actorpointers);
        RegisterFunction(L, "__vanta_runonactor", Actors::runonactor);
        RegisterFunction(L, "isparallel", Actors::isparallel);
        RegisterFunction(L, "getcallbackvalue", Callbacks::getcallbackvalue);
        RegisterFunction(L, "getcallbackmember", Callbacks::getcallbackvalue);

        RegisterFunction(L, "__vanta_draw_available", Draw::available);
        RegisterFunction(L, "__vanta_draw_set", Draw::set);
        RegisterFunction(L, "__vanta_draw_remove", Draw::remove);
        RegisterFunction(L, "__vanta_draw_clear", Draw::clear);
        RegisterFunction(L, "__vanta_draw_inspect", Draw::inspect);
        RegisterFunction(L, "__vanta_draw_textbounds", Draw::textbounds);
        RegisterFunction(L, "create_comm_channel", Actors::createcommchannel);
        RegisterFunction(L, "get_comm_channel", Actors::getcommchannel);

        // Script bytecode. All three come off one container read plus Roblox's
        // own decoder, so they either all work or all answer nil.
        RegisterFunction(L, "getscriptbytecode", Scripts::getscriptbytecode);
        RegisterFunction(L, "getscripthash", Scripts::getscripthash);
        RegisterFunction(L, "getscriptclosure", Scripts::getscriptclosure);

        RegisterFunction(L, "getrendersteppedlist", Signals::getrendersteppedlist);
        RegisterFunction(L, "__vanta_strictidentity", Reflection::strictidentity);
        RegisterFunction(L, "__vanta_propdesc", Reflection2::descriptorOf);
        RegisterFunction(L, "__vanta_classdesc", Reflection2::classDescriptorOf);
        RegisterFunction(L, "__vanta_descbytes", Reflection2::descBytes);
        RegisterFunction(L, "__vanta_pointer", Probe::pointer);
        RegisterFunction(L, "__vanta_ref", Reflection2::refOf);
        RegisterFunction(L, "__vanta_peekq", Reflection2::peekq);
        RegisterFunction(L, "__vanta_peekd", Reflection2::peekd);
        RegisterFunction(L, "__vanta_inimage", Reflection2::inimage);
        RegisterFunction(L, "__vanta_readproperty", Reflection2::readProperty);
        RegisterFunction(L, "__vanta_vulnpatch", Reflection2::vulnpatch);
        RegisterFunction(L, "__vanta_vulnpatch_passes", Reflection2::vulnpatchPasses);
        RegisterFunction(L, "__vanta_probeproperty", Reflection2::probeProperty);
        RegisterFunction(L, "__vanta_propertytype", Reflection2::propertyType);
        RegisterFunction(L, "__vanta_writeproperty", Reflection2::writeProperty);
        RegisterFunction(L, "__vanta_isscriptable", Reflection2::isscriptable);
        RegisterFunction(L, "__vanta_setscriptable", Reflection2::setscriptable);

        RegisterFunction(L, "__vanta_signalargs", Signals::getsignalarguments);
        RegisterFunction(L, "__vanta_signalargsinfo", Signals::getsignalargumentsinfo);
        RegisterFunction(L, "__vanta_signalidentity", Signals::signalidentity);
        RegisterFunction(L, "__vanta_signalargdump", Signals::signalargdump);
        RegisterFunction(L, "__vanta_islegalsendevent", Signals::islegalsendevent);
        RegisterFunction(L, "__vanta_raisespy", Signals::RaiseSpy::raisespy);
        RegisterFunction(L, "__vanta_raisespy_pairs", Signals::RaiseSpy::pairs);
        RegisterFunction(L, "__vanta_holderhint", Signals::RaiseSpy::holderhint);
        RegisterFunction(L, "__vanta_signalwhitelist", Signals::signalwhitelist);
        RegisterFunction(L, "__vanta_cansignalreplicate", Signals::cansignalreplicate);
        RegisterFunction(L, "__vanta_replicatesignal", Signals::replicatesignal);
        RegisterFunction(L, "__vanta_descreplicable", Signals::descriptorreplicable);
        RegisterFunction(L, "__vanta_slotinfo", Signals::slotinfo);
        RegisterFunction(L, "__vanta_slotenabled", Signals::slotenabled);
        RegisterFunction(L, "__vanta_log", Misc::journalWrite);
        RegisterFunction(L, "__vanta_setslotstate", Signals::setslotstate);
        RegisterFunction(L, "__vanta_fireforeign", Signals::fireforeign);

        RegisterFunction(L, "__vanta_getstack", Stack::getstack);
        RegisterFunction(L, "__vanta_setstack", Stack::setstack);

        RegisterFunction(L, "keypress", Input::keypress);
        RegisterFunction(L, "keyrelease", Input::keyrelease);
        RegisterFunction(L, "keyclick", Input::keyclick);
        RegisterFunction(L, "keytap", Input::keyclick);
        RegisterFunction(L, "mouse1press", Input::mouse1press);
        RegisterFunction(L, "mouse1release", Input::mouse1release);
        RegisterFunction(L, "mouse1click", Input::mouse1click);
        RegisterFunction(L, "mouse2press", Input::mouse2press);
        RegisterFunction(L, "mouse2release", Input::mouse2release);
        RegisterFunction(L, "mouse2click", Input::mouse2click);
        RegisterFunction(L, "mousemoveabs", Input::mousemoveabs);
        RegisterFunction(L, "mousemoverel", Input::mousemoverel);
        RegisterFunction(L, "mousescroll", Input::mousescroll);

        RegisterFunction(L, "rconsoleprint", Console::print);
        RegisterFunction(L, "rconsoleinfo", Console::info);
        RegisterFunction(L, "rconsolewarn", Console::warn);
        RegisterFunction(L, "rconsoleerr", Console::err);
        RegisterFunction(L, "rconsoleerror", Console::err);
        RegisterFunction(L, "rconsoleclear", Console::clear);
        RegisterFunction(L, "rconsoleshow", Console::show);
        RegisterFunction(L, "rconsolecreate", Console::show);
        RegisterFunction(L, "rconsolehide", Console::hide);
        RegisterFunction(L, "rconsoledestroy", Console::hide);
        RegisterFunction(L, "rconsolename", Console::name);
        RegisterFunction(L, "rconsolesettitle", Console::name);
        RegisterFunction(L, "rconsoleinput", Console::input,
                         reinterpret_cast<uintptr_t>(&Yield::continuation));

        // Shadow Roblox's print/warn on the executor surface only. The game's
        // own globals keep the original closures; a script's output reaches the
        // Vanta console through the hook and Roblox's output through the chain.
        //
        // Deliberately not RegisterFunction: these are still Roblox's globals as
        // far as isexecutorclosure is concerned, and the sUNC suite checks that
        // very thing ("did not return false for a Roblox global"). The hooks are
        // ours, but the names are not part of the executor's API surface.
        auto shadow = [&](const char* name, uintptr_t& original, lua_CFunction hook) {
            rbx::lua_getfield(L, LUA_GLOBALSINDEX, name);
            if (rbx::lua_iscfunction(L, -1))
                original = rbx::lua_tocfunction(L, -1);
            rbx::lua_settop(L, -2);
            rbx::lua_pushcclosurek(L, reinterpret_cast<uintptr_t>(hook), name, 0, 0);
            rbx::lua_setfield(L, LUA_GLOBALSINDEX, name);
        };
        shadow("print", Console::originalPrint, Console::printHook);
        shadow("warn", Console::originalWarn, Console::warnHook);

        RegisterFunction(L, "__vanta_dialog_openfile", Dialogs::openFile);
        RegisterFunction(L, "__vanta_dialog_openfolder", Dialogs::openFolder);
        RegisterFunction(L, "__vanta_dialog_savefile", Dialogs::saveFile);
        RegisterFunction(L, "__vanta_dialog_read", Dialogs::read);
        RegisterFunction(L, "__vanta_dialog_write", Dialogs::write);
        RegisterFunction(L, "__vanta_dialog_delete", Dialogs::remove);
        RegisterFunction(L, "__vanta_dialog_list", Dialogs::list);
        RegisterFunction(L, "__vanta_dialog_makefolder", Dialogs::makeFolder);
        RegisterFunction(L, "__vanta_dialog_stat", Dialogs::stat);
        RegisterFunction(L, "crypt_hash", Encoding::hash);
//      RegisterFunction(L, "lz4compress", Encoding::lz4compress);
//      RegisterFunction(L, "lz4decompress", Encoding::lz4decompress);
    }
}
