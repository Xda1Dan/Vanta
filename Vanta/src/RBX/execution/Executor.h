//
//  executor.hpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "luau/VM/include/lua.h"

#include "Offsets.h"
#include "ClientAPI.h"
#include "Compiler.h"
#include "LoadGuard.h"
#include "Chunks.h"
#include "LuaObject.h"
#include "Console.h"

inline uint64_t MaxCapabilities = 0xFFFFFFFFFFFFFFFFULL;

// One bit in the thread's capability word that means "the executor made this
// thread". It is how checkcaller answers, and it is RbxStu's design - its
// `LuauSecurity::IsOurThread` is a capability mark and nothing else, and it is
// the only one of the six that needs no set of raw pointers to answer. mac
// keeps `std::unordered_set<LuaTable*> OpiumwareThreads` of globals tables (as
// this tree did), and vic, src, skid and mod answer from the thread's script
// weak_ptr, which false-positives on every engine thread that has no script.
//
// Bit 6 is stu's own choice (`CapabilityPermissions::EXECUTOR_MARK = 0b1000000`)
// and it is free on this build too - measured with tests/caps.lua, which reads
// the word off every live thread:
//
//     engine script threads   identity 2   003fffffffffff00     (30 of them)
//     ours and our coroutines identity 8   203fffffffffff3f     (8 of them)
//
// so bit 6 is clear in both, and the low byte is the only part that differs by
// identity - which is why the mark cannot be one of the identity bits. It has
// to survive setthreadidentity lowering the identity, because that is exactly
// what sUNC does before asking checkcaller, so it is ORed in at every identity.
//
// An extra bit is inert to the engine: capability checks test
// `(capabilities & required) == required`, and this tree has been ORing in the
// 46 bits of 0x3FFFFFFFFFFF00 at every identity for as long as it has run.
inline constexpr uint64_t kExecutorMark = 0x40ULL;

inline void SetIdentity(uint64_t thread, uint32_t identity)
{
    uint64_t extraSpace = rbx::lua_getthreaddata(thread);
    *reinterpret_cast<uint32_t*>(extraSpace + offsets::identity) = identity;

    uint32_t capabilityIdentity = identity == 0 ? 8 : identity;
    uint64_t capabilities = rbx::identitytocapabilities(&capabilityIdentity)
        | 0x3FFFFFFFFFFF00ULL | kExecutorMark;
    *reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities) = capabilities;
}

// Whether a thread carries the mark. A coroutine of ours inherits the whole
// capability word from the thread that created it - verified in the same run -
// so this covers everything an executed script spawns, which is precisely the
// set the globals-table test was there to cover.
inline bool isExecutorThread(uint64_t thread)
{
    const uint64_t extraSpace = thread ? rbx::lua_getthreaddata(thread) : 0;
    if (!extraSpace)
        return false;
    return (*reinterpret_cast<uint64_t*>(extraSpace + offsets::capabilities) & kExecutorMark) != 0;
}

// Our threads inherit _G and shared straight from the game through the sandboxed
// globals table's __index, which means a script can see - and the game can see -
// that they are the same tables. SUNC calls that a detection vector outright, so
// each thread gets the executor's own pair instead, kept in the registry so every
// executed script shares one set rather than getting a fresh one each run.
inline void installExecutorGlobals(uint64_t thread)
{
    auto ensureTable = [&](const char* registryKey) {
        rbx::lua_getfield(thread, LUA_REGISTRYINDEX, registryKey);
        if (!rbx::lua_toboolean(thread, -1)) {
            rbx::lua_settop(thread, -2);            // drop the nil
            rbx::lua_createtable(thread, 0, 0);
            rbx::lua_pushvalue(thread, -1);         // one copy to store, one to use
            rbx::lua_setfield(thread, LUA_REGISTRYINDEX, registryKey);
        }
    };

    // luaL_sandboxthread marks the new globals table safeenv, which lets the VM
    // cache global reads. Writing to it without clearing that would leave those
    // caches pointing at the old values.
    rbx::lua_setsafeenv(thread, LUA_GLOBALSINDEX, 0);

    ensureTable("vanta._G");
    rbx::lua_setfield(thread, LUA_GLOBALSINDEX, "_G");

    ensureTable("vanta.shared");
    rbx::lua_setfield(thread, LUA_GLOBALSINDEX, "shared");
}

// The dedicated executor thread. All executor globals - getgenv, hookfunction,
// getrawmetatable and the ~200 others - live on THIS thread's sandboxed globals
// table, not on the game's. A game LocalScript is sandboxed against the game's
// own globals table and only ever chains to that, so it never sees any of them;
// an executed script is a child of this thread (see ExecuteScript) and chains
// through here, so it sees the whole surface. getgenv() returns this table
// automatically - a C closure's LUA_ENVIRONINDEX is the globals table it was
// registered on - so every executed script shares one executor environment.
//
// This is the isolation every reference relies on and the one thing this tree
// was missing: mac creates `MainThread = lua_newthread(GetGlobalState(...))`
// then `luaL_sandboxthread` + `LoadEnvironment`, Vicna an `ExploitThread`,
// sourcessss `our_state`, SkidBase and Module-Source an `exploitThread`, all
// `lua_newthread(gameState)` + `luaL_sandboxthread`. Registering straight onto
// the game state instead put getgenv on the table game scripts read, which is a
// blunt detection: Adonis's Anti module crashes any client whose script
// environment exposes getgenv (Client/Core/Anti.luau, `currentEnv.getgenv`).
inline uint64_t makeExecThread(uint64_t g_state)
{
    // getrenv answers from here: the game's real root globals table. Captured
    // from the game state, before the sandboxed executor table exists. Register
    // used to capture this itself (it was called with the game state), but it now
    // registers onto the executor thread, whose globals table is the executor
    // surface - so getrenv would otherwise hand a game script's worth of code
    // back the executor environment, defeating the whole point of getrenv.
    rbx::lua_pushvalue(g_state, LUA_GLOBALSINDEX);
    rbx::lua_setfield(g_state, LUA_REGISTRYINDEX, "vanta.renv");

    uint64_t thread = rbx::lua_newthread(g_state);
    // Anchor it for the life of this game. mac does the same (`lua_ref` and
    // never unref); the ref lives in this VM's registry and dies with the state
    // when the game ends, so a real teleport builds a fresh one on the new state.
    rbx::lua_ref(g_state, -1);
    rbx::lua_settop(g_state, -2);

    // A new globals table whose __index chains to the game's - so `game`,
    // `workspace`, `print` still resolve - but which is a table game scripts do
    // not read. SetIdentity marks it (and its children) as ours; installExecutor-
    // Globals clears safeenv so the coming Register writes are not shadowed by a
    // stale global-read cache, and gives it the executor's own _G/shared.
    rbx::luaL_sandboxthread(thread);
    SetIdentity(thread, 8);
    installExecutorGlobals(thread);
    return thread;
}

// Loads already-compiled bytecode on a fresh sandboxed child of `state_ptr` and
// hands it to the scheduler. Everything ExecuteScript does except the compile,
// split out so a chunk whose source never changes can skip that step.
inline void ExecuteBytecode(uint64_t state_ptr, const std::string& bc)
{
    auto top = rbx::lua_gettop(state_ptr);
    uint64_t rbx_thread = rbx::lua_newthread(state_ptr); 
    int threadRef = rbx::lua_ref(state_ptr, -1);
    rbx::lua_settop(state_ptr, -2);
    
    rbx::luaL_sandboxthread(rbx_thread);
    SetIdentity(rbx_thread, 8);
    installExecutorGlobals(rbx_thread);

    // Everything the executor loads is named "@vanta", which is what makes
    // isexecutorclosure able to answer for the Lua closures inside it.
    chunks::mark("@vanta");

    // safeenv must be ON for the load, and this is not cosmetic: luau_load
    // resolves every GETIMPORT constant eagerly *only* when the globals table is
    // safeenv (`if (L->gt->safeenv)` in lvmload.cpp) and otherwise writes a nil
    // in its place, leaving the lookup to happen at run time against whatever
    // environment is current by then. A script that sandboxes its own functions
    // with setfenv - which is exactly what a Luraph VM does - then resolves
    // `bit32.band`, `string.sub` and the rest through *its* table instead of the
    // real libraries, and misbehaves in a way no error reports.
    //
    // luaL_sandboxthread leaves the fresh table safeenv, and installExecutorGlobals
    // then clears it so its own writes are not shadowed by a stale cache - correct
    // for those writes, but it has to go back on before the chunk is loaded.
    // Writing a new key clears the flag by itself (`t->safeenv = 0` in ltable.cpp),
    // so this has to be the last thing before luau_load. Opiumware's Execute()
    // never clears it on the execution thread at all.
    rbx::lua_setsafeenv(rbx_thread, LUA_GLOBALSINDEX, 1);

    int loaded;
    {
        // Claim the buffer so Roblox's signature check lets it through.
        load::Ours chunk(bc.c_str());
        loaded = rbx::luau_load(rbx_thread, "@vanta", bc.c_str(), bc.length(), 0);
    }

    if (loaded != 0) {
        size_t errorSize = 0;
        const char* error = rbx::lua_tolstring(rbx_thread, -1, &errorSize);
        rbx::print(3, "Failed to load bytecode: %.*s", static_cast<int>(errorSize), error ? error : "");
        console::send("rconsoleerror",
                      "Failed to load bytecode: " + std::string(error ? error : "",
                                                               error ? errorSize : 0));
        rbx::lua_settop(rbx_thread, 0);
        rbx::lua_unref(state_ptr, threadRef);
        rbx::lua_settop(state_ptr, top);
        return;
    }
    
    rbx::setprotocapabilities(rbx_thread, -1, &MaxCapabilities);

    // The thread is anchored by `threadRef` only while it is being set up, and
    // released the moment the scheduler owns it. mac and stu do the same;
    // Vicna never refs the thread at all (`lua_newthread` then `lua_pop`), and
    // skid and mod stop the collector for the whole of their setup instead.
    rbx::task::spawn(rbx_thread);
    // The reference is deliberately *not* released here, and the reason is
    // worth writing down.
    //
    // task::spawn hands the thread to the scheduler, which resumes it on a
    // later frame. Releasing the registry reference the instant after handing
    // it over is a race: until the scheduler has taken a reference of its own,
    // the only thing keeping the thread alive is this one, and a collection in
    // between frees a thread that is about to be resumed.
    //
    // The race never fired while the header's `lua_unref` was wrong. It pointed
    // at a seven-instruction thunk a few bytes from the real one, so the
    // release was a no-op and the thread stayed anchored for ever - a slow
    // registry leak nobody noticed, and the thing that made this line look
    // correct. The offset dumper derives the real lua_unref (its body matches
    // Luau's at 0.99 where the thunk scores 0.45, and releasing a reference
    // really does hand the same number back out of lua_ref, which the thunk
    // does not), and with the real address in the header this call takes the
    // client down inside ExecuteScript on a one-line script.
    //
    // Leaking one registry slot per executed chunk is the right trade: the
    // alternative is releasing it a frame later, which needs a frame.
    (void)threadRef;
    rbx::lua_settop(state_ptr, top);
}

void ExecuteScript(uint64_t state_ptr, const std::string& script)
{
    if (script.empty())
        return;
    ExecuteBytecode(state_ptr, compileScript(script));
}

// For the bootstrap chunks only. Their source is a module-static string that is
// byte-identical on every join, and compiling the 127 KB namecall chunk cost
// ~8.9 ms per join (journal, 2026-09-03) - so the bytecode is compiled once per
// process and reused. Import resolution (GETIMPORT baking) happens in luau_load,
// not in the compiler, so a cached chunk still binds against the new join's
// environment. Keyed by the source string's ADDRESS: only ever pass the static
// unc::kXxx strings here - a per-join temporary would leak an entry per call.
inline void ExecuteScriptCached(uint64_t state_ptr, const std::string& source)
{
    if (source.empty())
        return;
    static std::mutex mutex;
    static std::unordered_map<const std::string*, std::string> cache;
    const std::string* bc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(&source);
        if (it == cache.end())
            it = cache.emplace(&source, compileScript(source)).first;
        bc = &it->second;
    }
    ExecuteBytecode(state_ptr, *bc);
}
