//
//  rbx.hpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//

#pragma once

#include <cstdint>
#include <string>
#include "Offsets.h"

// These derive from the offsets:: constants, so they must be initialized after
// them. `inline` variables are only partially ordered with respect to ordinary
// namespace-scope ones (C++17 [basic.start.dynamic]), and clang runs them
// first - every pointer then captures a zero. `static` gives internal linkage
// and ordered initialization, which puts them after offsets:: as intended.
namespace rbx {
    // LuaU
    static uint64_t (*lua_newthread)(uint64_t) = reinterpret_cast<uint64_t (*)(uint64_t)>(offsets::lua_newthread);
    static int (*luau_load)(uint64_t, const char *, const char *, size_t, int) = reinterpret_cast<int (*)(uint64_t, const char *, const char *, size_t, int)>(offsets::luau_load);
    static int (*lua_gettop)(uint64_t) = reinterpret_cast<int (*)(uint64_t)>(offsets::lua_gettop);
    static void (*lua_settop)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_settop);
    static const char* (*lua_tolstring)(uint64_t, int, size_t*) = reinterpret_cast<const char* (*)(uint64_t, int, size_t*)>(offsets::lua_tolstring);
    static int (*lua_tointegerx)(uint64_t, int, int*) = reinterpret_cast<int (*)(uint64_t, int, int*)>(offsets::lua_tointegerx);
    static int (*lua_toboolean)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_toboolean);
    static void (*lua_createtable)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_createtable);
    static void (*lua_rawseti)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_rawseti);
    static int (*lua_setmetatable)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_setmetatable);
    static void (*lua_pushinteger)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_pushinteger);
    static void (*lua_setsafeenv)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_setsafeenv);
    static int (*lua_getfield)(uint64_t, int, const char*) = reinterpret_cast<int (*)(uint64_t, int, const char*)>(offsets::lua_getfield);
    static int (*lua_iscfunction)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_iscfunction);
    static int (*lua_islfunction)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_islfunction);
    static void (*lua_pushboolean)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_pushboolean);
    static int (*lua_ref)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_ref);
    static void (*lua_unref)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_unref);
    // inline int (*lua_pcall)(uint64_t, int, int, int) = reinterpret_cast<int (*)(uint64_t, int, int, int)>(offsets::lua_pcall);
    static void (*lua_pushcclosurek)(uint64_t, uintptr_t, const char *, int, uintptr_t) = reinterpret_cast<void (*)(uint64_t, uintptr_t, const char *, int, uintptr_t)>(offsets::lua_pushcclosurek);
    static void (*lua_pushlstring)(uint64_t, const char*, size_t) = reinterpret_cast<void (*)(uint64_t, const char*, size_t)>(offsets::lua_pushlstring);
    static void (*lua_pushnil)(uint64_t) = reinterpret_cast<void (*)(uint64_t)>(offsets::lua_pushnil);
    static void (*lua_pushvalue)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_pushvalue);
    // inline void (*lua_insert)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_insert);
    static void (*lua_setfield)(uint64_t, int, const char*) = reinterpret_cast<void (*)(uint64_t, int, const char*)>(offsets::lua_setfield);
    // inline void (*lua_setsafeenv)(uint64_t, int, bool) = reinterpret_cast<void (*)(uint64_t, int, bool)>(offsets::lua_setsafeenv);
    static uint64_t (*lua_getthreaddata)(uint64_t) = reinterpret_cast<uint64_t (*)(uint64_t)>(offsets::lua_getthreaddata);
    static void (*luaL_sandboxthread)(uint64_t) = reinterpret_cast<void (*)(uint64_t)>(offsets::luaL_sandboxthread);
    static int (*lua_type)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_type);
    static void* (*lua_touserdata)(uint64_t, int) = reinterpret_cast<void* (*)(uint64_t, int)>(offsets::lua_touserdata);
    static void (*lua_pushlightuserdatatagged)(uint64_t, void*, int) = reinterpret_cast<void (*)(uint64_t, void*, int)>(offsets::lua_pushlightuserdatatagged);
    static int (*luaL_checktype)(uint64_t, int, int) = reinterpret_cast<int (*)(uint64_t, int, int)>(offsets::luaL_checktype);
    static int (*lua_getmetatable)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_getmetatable);
    static void (*lua_setreadonly)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_setreadonly);
    static int (*lua_getreadonly)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_getreadonly);
    static const char* (*lua_namecallatom)(uint64_t, int*) = reinterpret_cast<const char* (*)(uint64_t, int*)>(offsets::lua_namecallatom);
    static void (*lua_error)(uint64_t) = reinterpret_cast<void (*)(uint64_t)>(offsets::lua_error);
    static void (*lua_insert)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_insert);
    static void (*lua_remove)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_remove);
    static uintptr_t (*lua_tocfunction)(uint64_t, int) = reinterpret_cast<uintptr_t (*)(uint64_t, int)>(offsets::lua_tocfunction);
    static const void* (*lua_topointer)(uint64_t, int) = reinterpret_cast<const void* (*)(uint64_t, int)>(offsets::lua_topointer);
    static void (*lua_getfenv)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_getfenv);
    static int (*lua_setfenv)(uint64_t, int) = reinterpret_cast<int (*)(uint64_t, int)>(offsets::lua_setfenv);
    static void (*lua_call)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_call);
    static void (*lua_rawgeti)(uint64_t, int, int) = reinterpret_cast<void (*)(uint64_t, int, int)>(offsets::lua_rawgeti);
    static void (*lua_rawset)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_rawset);
    static int (*lua_gc)(uint64_t, int, int) = reinterpret_cast<int (*)(uint64_t, int, int)>(offsets::lua_gc);
    static void (*luaM_visitgco)(uint64_t, void*, bool (*)(void*, void*, void*)) = reinterpret_cast<void (*)(uint64_t, void*, bool (*)(void*, void*, void*))>(offsets::luaM_visitgco);
    static void* (*luaF_newLclosure)(uint64_t, int, void*, void*) = reinterpret_cast<void* (*)(uint64_t, int, void*, void*)>(offsets::luaF_newLclosure);
    static void (*lua_clonefunction)(uint64_t, int) = reinterpret_cast<void (*)(uint64_t, int)>(offsets::lua_clonefunction);

    // Roblox
    static void (*print)(int, const char *, ...) = reinterpret_cast<void (*)(int, const char *, ...)>(offsets::print);
    static uint64_t (*getgstate)(uint64_t, uint64_t*, uint64_t*) = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t*, uint64_t*)>(offsets::getglobalstate);
    static uint64_t (*identitytocapabilities)(uint32_t*) = reinterpret_cast<uint64_t (*)(uint32_t*)>(offsets::identitytocapabilities);
    // The thread-local security context the engine reads back, as opposed to
    // the copy in the lua_State's extra space. See offsets::identitypropagator.
    static uintptr_t (*identitypropagator)(uint64_t) = reinterpret_cast<uintptr_t (*)(uint64_t)>(offsets::identitypropagator);
    static uint64_t (*setprotocapabilities)(uint64_t, int, uint64_t*) = reinterpret_cast<uint64_t (*)(uint64_t, int, uint64_t*)>(offsets::setprotocapabilities);

    // Roblox's own bytecode container decoder. Result is {std::string, bool},
    // so 32 bytes of storage with the flag right after the string.
    struct DecompressResult {
        char storage[24];
        bool ok;
    };
    static void (*decompressbytecode)(DecompressResult*, const std::string*) =
        reinterpret_cast<void (*)(DecompressResult*, const std::string*)>(offsets::decompressbytecode);

    // The 16-byte weak reference an Instance userdata holds.
    struct WeakInstance {
        uintptr_t object;
        uintptr_t control;
    };
    static void (*pushinstance)(uint64_t, WeakInstance*) =
        reinterpret_cast<void (*)(uint64_t, WeakInstance*)>(offsets::pushinstance);

    namespace task {
        static uint64_t (*spawn)(uint64_t) = reinterpret_cast<uint64_t (*)(uint64_t)>(offsets::taskspawn);
    }
}
