//
//  main.cpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//

#include <cstdio>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <dlfcn.h>
#include <algorithm>
#include <filesystem>
#include <vector>

#include "tinyhook/include/tinyhook.h"

#include "luau/VM/include/lua.h"
#include "Offsets.h"
#include "ClientAPI.h"
#include "Verify.h"
#include "LoadClock.h"
#include "TaskScheduler.h"
#include "Status.h"
#include "Executor.h"
#include "Overlay.h"
#include "Socket.h"
#include "ControlServer.h"
#include "Journal.h"
#include "Filesystem.h"
#include "Registry.h"

std::atomic<uint64_t> global_state{0};

int64_t (*playerleavehook)(int64_t, int64_t);
int64_t hook2(std::int64_t a1, std::int64_t a2) {
    journal::write("LEAVE player left");
    // Stop the pump before anything else. Between here and the next Register
    // the DataModel is on its way out, and a Heartbeat that fires on the way
    // would load a queued script into a state that is being freed.
    unc::Pump::disarm();
    runtime::set(runtime::State::waiting, "left the game; waiting for the next one");
    global_state.store(0, std::memory_order_release);
    return playerleavehook(a1, a2);
}

int64_t (*teleporthook)(int64_t, uint64_t*, uint64_t*);
int64_t hook3(int64_t a1, uint64_t* a2, uint64_t* a3) {
    journal::write("TELEPORT player teleported");
    runtime::set(runtime::State::waiting, "teleporting; waiting for the next game");
    // Nothing is armed here any more. The queue used to be flagged as pending
    // from inside this hook, which meant a synchronous open/write/close on
    // Roblox's own thread in the middle of a teleport - and `teleport::take()`
    // stopped reading that flag when the drain became unconditional, so the
    // write bought nothing at all.
    unc::Pump::disarm();
    global_state.store(0, std::memory_order_release);
    return teleporthook(a1, a2, a3);
}

// Patching an address that is no longer a function entry writes a jump into the
// middle of live code, which takes Roblox down the moment that code runs.
bool hookChecked(const char* name, uintptr_t target, void* replacement, void** original) {
    if (!verify::isFunctionEntry(target)) {
        printf("[vanta] skipping %s hook: 0x%llx is not a function entry\n",
               name, (unsigned long long)target);
        fflush(stdout);
        // stdout goes nowhere under `open`, and a silently skipped signature
        // hook looks exactly like a working one until a large script fails.
        journal::writef("HOOK skipped %s: 0x%llx is not a function entry",
                        name, (unsigned long long)target);
        return false;
    }
    int rc = tiny_hook((void *)target, replacement, original);
    printf("[vanta] hooked %s at 0x%llx (rc=%d)\n", name, (unsigned long long)target, rc);
    fflush(stdout);
    if (rc != 0)
        journal::writef("HOOK failed %s at 0x%llx (rc=%d)",
                        name, (unsigned long long)target, rc);
    return rc == 0;
}

// Roblox is pinned to the build every offset in this file was derived against,
// and RobloxPlayerInstaller.app is gone from the bundle, so the client cannot
// update even though its channel has moved past it. Left alone the Lua app
// shell notices and puts a modal "Update Required - Roblox needs to restart to
// update" over the home screen on every launch, with nothing behind the button
// but "Close Roblox".
//
// The engine has a supported way to answer that without hooking anything:
// `AppUpdateAdapter::checkForUpdate` returns FString::DebugAppUpdateStatus
// verbatim when it is set, before it queries the version endpoint at all.
// Writing "NotAvailable" there makes every caller - the app shell included -
// be told the client is current.
//
// The macOS reference does the same job the other way round, replacing
// AppUpdateService::checkForUpdate (its `UpdaterFunction`, found by the same
// FLog string) and calling the Lua callback with
// Enum.AppUpdateStatus.NotAvailable by hand. That works, but it needs the
// WeakObjectRef/ObjectRef/LiveThreadRef layouts and rebuilds a call the engine
// would have made anyway; this is the same answer in one write, on Roblox's
// own code path, with nothing to keep in step when the layouts move. None of
// the other five references touch the update check at all.
void silenceUpdatePrompt() {
    // libc++'s short-string form: byte 0 is (length << 1), the low bit left
    // clear to mark it short, and the characters follow in place. Twelve is
    // well inside the 22 a short string holds, so there is no allocation to
    // own and the value stays good for the life of the process.
    static const char status[] = "NotAvailable";
    unsigned char value[24] = {};
    value[0] = static_cast<unsigned char>((sizeof(status) - 1) << 1);
    std::memcpy(value + 1, status, sizeof(status) - 1);

    // A stale offset here would not fault - __DATA is writable - it would
    // quietly scribble 24 bytes over whatever moved into the address, which is
    // the worst way for a wrong offset to be wrong. The flag has never been set
    // on a launch, and it lives in __common, so the only shape that can be
    // correct is 24 zero bytes; anything else means this is no longer the
    // string and the write is refused.
    unsigned char current[24] = {};
    if (!luaobj::safeRead(current, offsets::debugappupdatestatus, sizeof(current))) {
        journal::write("UPDATE DebugAppUpdateStatus is unreadable; leaving the check alone");
        return;
    }
    for (unsigned char byte : current) {
        if (byte != 0) {
            journal::write("UPDATE DebugAppUpdateStatus is not an empty string; "
                           "the offset needs re-deriving and nothing was written");
            return;
        }
    }

    if (luaobj::safeWrite(offsets::debugappupdatestatus, value, sizeof(value)))
        journal::write("UPDATE app update check answered NotAvailable");
    else
        journal::write("UPDATE could not write DebugAppUpdateStatus");
}

void hookall() {
    // Before the hooks: this is a plain data write and the app shell asks the
    // question within a few seconds of launch.
    silenceUpdatePrompt();

    // Without this the executor can only run chunks that compile to under 280
    // bytes of bytecode; everything larger comes back "bytecode corrupted".
    hookChecked("bytecode signature", offsets::checkbytecodesignature,
                (void *)&signature::check, (void **)&signature::original);
    hookChecked("playerleave", offsets::hook_pl, (void *)&hook2, (void **)&playerleavehook);
    hookChecked("teleport", offsets::hook_tp, (void *)&hook3, (void **)&teleporthook);

    // The four script-integrity flags the stats ping item sends, answered as
    // the clean client answers them. gameldr.hpp explains why that is a 7 and
    // why nothing depends on this: the load clock and the capture operand keep
    // all four clean on their own, and tests/detect.lua measures them at the
    // source rather than through here.
    hookChecked("detection status", offsets::detectionstatus,
                (void *)&gameldr::status, (void **)&gameldr::statusOriginal);
    return;
}


// Every plain-text file in ~/Documents/Vanta/autoexec, in name order.
//
// The folder is its own thing - a sibling of the workspace, not a directory
// inside it - and it is created on first use so it is discoverable. Extension
// is not the filter: whatever is in there runs, as long as it is text. A binary
// file is skipped rather than fed to the compiler, since the only thing that
// could produce is a syntax error against garbage.
bool looksLikeText(const std::string& source) {
    // A NUL byte is what actually separates a compiled or binary file from a
    // script. Luau source may otherwise hold any byte a string literal can, so
    // nothing narrower would be honest about "plain text".
    return source.find('\0') == std::string::npos;
}

// The GUI's autoexec switches.
//
// They have to be on disk and readable from here: the dylib reads the folder at
// join, with no GUI attached and no socket up yet, so UserDefaults is no use.
// One line-based file rather than JSON - a master switch and a list of names
// does not justify a parser dependency:
//
//     enabled=1
//     off=esp.lua
//
// It lives *inside* the autoexec folder so it travels with it, which is safe
// because the listing below already skips dotfiles - so it can never run
// itself. Absent means "on, nothing disabled".
struct AutoexecConfig {
    bool enabled = true;
    std::vector<std::string> off;

    bool isOff(const std::string& name) const {
        return std::find(off.begin(), off.end(), name) != off.end();
    }
};

AutoexecConfig readAutoexecConfig() {
    AutoexecConfig config;
    std::ifstream file(workspace::autoexecRoot() / ".vanta-autoexec");
    if (!file)
        return config;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind("enabled=", 0) == 0)
            config.enabled = line.substr(8) != "0";
        else if (line.rfind("off=", 0) == 0)
            config.off.push_back(line.substr(4));
    }
    return config;
}

// The GUI's executor switches that are not about autoexec.
//
// Same shape and the same reasoning as .vanta-autoexec above - the dylib reads
// it at join, with no GUI attached and no socket up, so the state has to be on
// disk and parseable from here - but it sits beside the workspace instead of
// inside the autoexec folder, because it is not about autoexec and because a
// script must not be able to write it (see workspace::settingsFile).
//
//     ~/Documents/Vanta/.vanta-settings
//     safemode=1
//
// Absent means safe mode on, which is what every build up to now has done.
struct ExecutorSettings {
    bool safeMode = true;
};

ExecutorSettings readExecutorSettings() {
    ExecutorSettings settings;
    std::ifstream file(workspace::settingsFile());
    if (!file)
        return settings;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind("safemode=", 0) == 0)
            settings.safeMode = line.substr(9) != "0";
    }
    return settings;
}

std::vector<std::string> loadAutoexecScripts() {
    std::vector<std::string> scripts;
    const std::filesystem::path folder = workspace::autoexecRoot();
    std::error_code code;

    // Re-read every join, so toggling a switch in the GUI takes effect on the
    // next join without restarting anything.
    const AutoexecConfig config = readAutoexecConfig();
    if (!config.enabled) {
        journal::write("AUTOEXEC disabled in settings; nothing will run");
        return scripts;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, code)) {
        if (!entry.is_regular_file(code))
            continue;
        // Finder leaves .DS_Store in every directory it has been shown, and it
        // is not a script.
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.')
            continue;
        // Turned off from the explorer. Skipped rather than deleted, so it is
        // still there to turn back on.
        if (config.isOff(name))
            continue;
        files.push_back(entry.path());
    }
    // Name order, so a user can decide what runs first by naming the files.
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& path : files) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        std::string source = contents.str();
        if (source.empty())
            continue;
        if (!looksLikeText(source)) {
            printf("[vanta] autoexec: skipping %s (not text)\n",
                   path.filename().string().c_str());
            fflush(stdout);
            continue;
        }
        printf("[vanta] autoexec: %s (%zu bytes)\n", path.filename().string().c_str(),
               source.size());
        fflush(stdout);
        scripts.push_back(std::move(source));
    }
    return scripts;
}

// Wait for a game that is not the one we just left.
//
// `skip` is the ScriptContext the previous round was built on. Immediately
// after a teleport the old context is still in the job list and still has a
// place id for a frame or two, so waiting for "any game" hands back the one
// that is going away - we build the whole environment on it, and the change
// detection in the loop below has to tear it down and do it again a second
// later. Refusing the one we just left costs nothing and skips that round trip.
//
// Polled five times a second rather than once: the second between a game coming
// up and us noticing is a second in which anything queued has nowhere to run.
uintptr_t waitForGame(uintptr_t skip) {
    // Refusing the context we just left is an optimisation, not a rule. The
    // allocator is free to hand the next DataModel a ScriptContext at the very
    // address the last one had, and waiting for a *different* pointer would
    // then wait for ever - so after a few seconds of seeing only that address,
    // take it. A wasted rebuild is cheap; a client that never regains execution
    // is the bug this whole file is about.
    // 50 ms between looks, not 200. The hook-to-JOIN gap measured 0.43-1.65 s
    // on a teleport (journal, 2026-09-03), most of it Roblox's own load, and a
    // 200 ms poll added up to 200 ms of pure waiting on top; the job walk is a
    // handful of guarded reads, so twenty of them a second costs nothing a
    // frame could notice. The insist window stays at 5 s and the job dump at
    // every 10 s - both are counted in polls, so they scale with it.
    constexpr int kPollMs = 50;
    constexpr int kInsistFor = 5000 / kPollMs;      // 5 s
    constexpr int kDumpEvery = 10000 / kPollMs;     // 10 s

    for (int tries = 0, sameSeen = 0;; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        const uintptr_t context = taskscheduler::getScriptContext();
        if (!context) {
            sameSeen = 0;
            if (tries % kDumpEvery == kDumpEvery - 1)
                taskscheduler::dumpJobs();
            continue;
        }

        if (context == skip) {
            if (++sameSeen < kInsistFor)
                continue;
            journal::writef("JOIN script context %llx (same address as the last game; "
                            "taking it after %d ms)",
                            (unsigned long long)context, sameSeen * kPollMs);
        } else {
            journal::writef("JOIN script context %llx", (unsigned long long)context);
        }

        // Nothing is printed to Roblox's console on join. A "loaded" banner (or
        // any executor chatter) lands in FLog::CreatorOutput / LogService, which
        // anti-cheats scan - Adonis's Anti_Cheat plugin watches MessageOut for
        // exactly this kind of signature - and no reference prints one; they log
        // to their own console. The join is recorded in our journal above.
        return context;
    }
}

__attribute__((constructor)) void init()
{
    // DYLD_INSERT_LIBRARIES is inherited by RobloxCrashHandler and RobloxMenuBar,
    // where none of these offsets mean anything.
    if (!hostImage().found)
        return;

    // A liveness probe (the GUI's socketIsListening, every 1.5s) connects and
    // closes without completing the handshake, so the server's first write - the
    // nonce it now sends on every connection - lands on a peer that has already
    // gone. That write raises SIGPIPE, and with no handler the DEFAULT action
    // terminates the whole client (RobloxPlayer exits 141 = 128+SIGPIPE). The
    // per-socket SO_NOSIGPIPE does not cover it here, so ignore SIGPIPE for the
    // process: every socket write already checks its return value.
    signal(SIGPIPE, SIG_IGN);

    printf("hi lol\n");
    runtime::set(runtime::State::starting, "installing hooks");

    // A watchdog that owes nothing to Roblox. It runs on a thread of its own and
    // writes to the journal once a second, so a run where the Lua heartbeat
    // stops can be told apart from a run where the whole process stopped: if
    // these lines keep coming while the heartbeat does not, the client is alive
    // and it is Roblox's task scheduler that has stalled.
    std::thread([]() {
        for (uint64_t tick = 0;; tick++) {
            journal::writef("WATCHDOG %llu", static_cast<unsigned long long>(tick));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();

    std::thread([]() {
        // The loopback HTTP server is the GUI's control channel now; anything
        // queued while the menu is up runs on the next join.
        control::start();

        std::this_thread::sleep_for(std::chrono::seconds(1));
        // A null here means the offsets:: constants had not been initialized yet
        // when these were resolved; calling one jumps straight to address zero.
        if (!rbx::print || !rbx::getgstate || !rbx::lua_newthread || !rbx::luau_load ||
            !rbx::task::spawn || !rbx::lua_gettop || !rbx::lua_settop || !rbx::lua_ref ||
            !rbx::lua_unref || !rbx::luaL_sandboxthread || !rbx::lua_getthreaddata ||
            !rbx::identitytocapabilities || !rbx::setprotocapabilities ||
            !rbx::lua_pushcclosurek || !rbx::lua_setfield || !rbx::lua_tolstring ||
            !rbx::lua_iscfunction || !rbx::lua_pushboolean || !rbx::lua_islfunction ||
            !rbx::lua_pushvalue) {
            printf("[vanta] rbx function pointers are unresolved; standing down\n");
            fflush(stdout);
            runtime::set(runtime::State::failed,
                         "dylib functions are unresolved for this Roblox build");
            return;
        }

        if (verify::reportStaleOffsets() > 0) {
            // The GUI surfaces this as REINSTALL: the client is on a build
            // these offsets were not derived from, and nothing below can work.
            journal::write("REINSTALL stale-offsets: Roblox is not the pinned build");
            printf("[vanta] offsets need re-dumping for this Roblox build; standing down\n");
            fflush(stdout);
            runtime::set(runtime::State::reinstall,
                         "Roblox is not the pinned build; reinstall it to match these offsets");
            return;
        }
        hookall();
        // Bring the Drawing overlay's window up now rather than on first use:
        // it has to be built on the main queue, and doing that while a script
        // waits on the answer is the one thing that could stall a script.
        overlay::start();
        runtime::set(runtime::State::waiting, "waiting for a game");
        uintptr_t previousContext = 0;
        uintptr_t previousDataModel = 0;
        while (true) {
            const uintptr_t ScriptContext = waitForGame(previousContext);
            const uintptr_t dataModel = taskscheduler::dataModelOf(ScriptContext);

            // A teleport that never happened.
            //
            // `TeleportService:Teleport` can be refused - Roblox rate-limits it,
            // and a hop can simply fail - but the teleport hook has already
            // fired by then, so the loop is here with the *same* DataModel it
            // was already running in. Rebuilding would tear down and re-register
            // everything for a game that never changed, which throws away every
            // hook and wrapper a script had installed. Nothing needs rebuilding;
            // the pump just needs arming again.
            const bool sameGame = dataModel != 0 && ScriptContext == previousContext &&
                                  dataModel == previousDataModel;
            previousContext = ScriptContext;
            previousDataModel = dataModel;

            if (sameGame) {
                journal::writef("RESUME same DataModel %llx - the teleport did not happen; "
                                "keeping the environment",
                                (unsigned long long)dataModel);
                global_state.store(unc::mainState, std::memory_order_release);
                unc::Pump::arm();
                runtime::set(runtime::State::ready,
                             "same game as before; environment kept and pump armed");
            } else {

            uint64_t t1 = 0;
            uint64_t t2 = 0;
            auto g_state = rbx::getgstate(ScriptContext, &t1, &t2);
            global_state.store(g_state, std::memory_order_release);
            
            // Let the engine's own `require` work for us. Without this every
            // require from an executor thread dies on "Cannot require a
            // non-RobloxScript module from a RobloxScript" - the check is on
            // the caller, not the module, so it fails for every module in the
            // place. Vicna sets exactly this flag once at setup for the same
            // reason. It goes back to 0 with the ScriptContext when the game
            // ends, so it is set again on every join.
            if (ScriptContext && offsets::scriptcontext_requirebypass) {
                *reinterpret_cast<uint8_t*>(ScriptContext + offsets::scriptcontext_requirebypass) = 1;
                journal::writef("REQUIRE bypass set on ScriptContext %llx",
                                (unsigned long long)ScriptContext);
            } else if (ScriptContext) {
                journal::writef("REQUIRE bypass offset is not derived for this build; skipped");
            }

            // The pump needs a way to run a script; give it one before the
            // bootstrap connects it to Heartbeat. It is handed bytecode: the
            // pump compiled the script when it was queued, off this thread.
            unc::runBytecode = [](uint64_t state, const std::string& bytecode) {
                ExecuteBytecode(state, bytecode);
            };

            // Everything the executor exposes goes on a dedicated sandboxed
            // thread's globals table, never the game's - so a game LocalScript
            // (an anti-cheat like Adonis included) that only chains to the game's
            // globals cannot see getgenv/hookfunction/etc. Register binds its
            // closures to this thread's table (that is what getgenv returns), and
            // every executed script is a child of it. See makeExecThread; this is
            // what mac's MainThread, Vicna's ExploitThread and the rest all do.
            // Each phase is timed into the journal. ExecuteScript compiles and
            // loads here and hands the chunk to the scheduler, so these numbers
            // are the compile+load cost on this thread; the bootstrap journals
            // its own run time from Lua once it has executed.
            const auto joinT0 = std::chrono::steady_clock::now();
            uint64_t execThread = makeExecThread(g_state);
            unc::Register(execThread);
            const auto joinT1 = std::chrono::steady_clock::now();
            // Puts HttpGet on the DataModel. Has to land before anything that
            // might call it, so it goes ahead of the teleport queue.
            ExecuteScriptCached(execThread, unc::kNamecallBootstrap);
            const auto joinT2 = std::chrono::steady_clock::now();

            // Apply the vulnerability blocklist through the reflection
            // descriptors before any queued or autoexec script can reach a
            // blocked member. Idempotent across joins (descriptors are global),
            // so re-running on a teleport is a no-op beyond the lookups.
            //
            // Safe mode is the GUI's switch over exactly this, re-read here on
            // every join so turning it off takes effect on the next join
            // without restarting anything - the contract the autoexec switches
            // already have. Both halves are needed: vulnpatch::enabled is what
            // the trampoline reads, and it has to be set either way because
            // descriptors patched on an earlier join of this process stay
            // patched; skipping the script only saves the ~220 lookups.
            vulnpatch::enabled = readExecutorSettings().safeMode;
            journal::writef("VULN safe mode %s", vulnpatch::enabled ? "on" : "off");
            if (vulnpatch::enabled)
                ExecuteScriptCached(execThread, unc::kVulnPatchScript);
            const auto joinT3 = std::chrono::steady_clock::now();

            // Last: fold the internal `__vanta_` primitives out of the enumerable
            // global surface. Runs after the two scripts above, which use them,
            // and re-runs each join because the C side re-registers them.
            ExecuteScriptCached(execThread, unc::kHideInternals);
            const auto joinT4 = std::chrono::steady_clock::now();
            {
                using std::chrono::duration_cast;
                using std::chrono::microseconds;
                journal::writef("JOIN setup: register %lld us, bootstrap compile+load %lld us, "
                                "vulnpatch compile+load %lld us, hideinternals compile+load %lld us",
                                (long long)duration_cast<microseconds>(joinT1 - joinT0).count(),
                                (long long)duration_cast<microseconds>(joinT2 - joinT1).count(),
                                (long long)duration_cast<microseconds>(joinT3 - joinT2).count(),
                                (long long)duration_cast<microseconds>(joinT4 - joinT3).count());
            }

            // Whatever queue_on_teleport left behind goes first, so a teleport
            // picks up where the last place left off before autoexec runs.
            //
            // Both are pushed with Gate::gameReady, so the pump holds them
            // until the bootstrap reports game:IsLoaded() *and* a LocalPlayer.
            // taskscheduler::isLoaded() only says the ScriptContext exists - a
            // queued script that runs on that alone is looking at a half-built
            // DataModel, and the things it wants (a character, a remote, a GUI)
            // may not be there yet.
            //
            // The gate replaced a `"repeat task.wait() until game:IsLoaded()\n"`
            // prefix on the teleport drain, which is what mac's hooks.hpp does:
            // prepending a line shifted every line number in every error either
            // a queued or an autoexec script could raise.
            // Everything below is *queued*, not run here. The bootstrap above
            // connected the pump to Heartbeat, so these are loaded and spawned
            // on Roblox's own thread on the frames that follow - which is what
            // every reference does, and what keeps this off the Lua heap while
            // the VM is using it. Order is preserved: the queue is a vector.
            for (const std::string& queued : unc::teleport::take()) {
                journal::writef("QUEUE teleport script (%zu bytes)", queued.size());
                unc::Pump::push(queued, unc::Pump::Gate::gameReady);
            }

            // autoexec runs on every join, after anything a teleport left
            // behind - a script queued for a hop is continuing what it was
            // doing, and should not be preceded by a fresh autoexec run.
            for (const std::string& script : loadAutoexecScripts())
                unc::Pump::push(script, unc::Pump::Gate::gameReady);

            runtime::set(runtime::State::ready,
                         "game joined; environment ready, scripts gated until it loads");
            }   // end of the "this is a different game" branch
            
            // Two things end this game, and until now only the first was
            // watched for.
            //
            //  * `global_state` going to zero, which the playerleave and
            //    teleport hooks do.
            //  * the scheduler simply having a different ScriptContext than the
            //    one we built on. That is what a server hop looks like when it
            //    does not go through either hooked function - and while it went
            //    unnoticed, every script after it was loaded into a lua_State
            //    belonging to a DataModel that no longer exists. "Losing
            //    execution after a rejoin" is that, and so is a crash on the
            //    next `lua_newthread` into freed memory.
            //
            // SkidBase's teleport handler watches for exactly this, polling the
            // place id and the global state and rebuilding when either moves.
            //
            // The wait is on the socket queue rather than a sleep: a script that
            // has already arrived is pushed to the pump straight away instead of
            // waiting out the rest of a 25 ms tick, and a client nobody is
            // sending to wakes four times a second instead of forty. The
            // timeout is what paces the job walk, which is the only thing here
            // that has to happen on a timer.
            while (global_state.load(std::memory_order_acquire) != 0) {
                execsock::waitForScript(std::chrono::milliseconds(250));

                std::string script;
                while (execsock::popScript(script)) {
                    journal::writef("QUEUE socket script (%zu bytes)", script.size());
                    unc::Pump::push(std::move(script));
                }

                const uintptr_t now = taskscheduler::getScriptContext();
                // A **zero** is "cannot tell right now", not "the game changed".
                // The place id reads 0 for a frame or two around a load, and
                // treating that as a new game tears the environment down
                // mid-session - every hook, wrapper and adopted thread cleared
                // out from under a running script. Only a different, real
                // context counts.
                if (now != 0 && now != ScriptContext) {
                    journal::writef("REBUILD the game changed: context %llx -> %llx",
                                    (unsigned long long)ScriptContext,
                                    (unsigned long long)now);
                    unc::Pump::disarm();
                    runtime::set(runtime::State::waiting, "the game changed; rebuilding");
                    global_state.store(0, std::memory_order_release);
                    break;
                }
            }
        }
    }).detach();
}
