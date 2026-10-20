//
//  compileopts.hpp
//  vanta-base
//
//  How a script is compiled - in one place, because two things compile now.
//
//  The executor loads every script with these options; tools/luaucheck.cpp
//  answers the editor's "is this an error" with the same ones. If the two ever
//  drift, the editor starts disagreeing with the thing that actually runs the
//  script, and a disagreement is a false error by definition.
//
#pragma once

#include "luau/Compiler/include/Luau/Compiler.h"

inline Luau::CompileOptions vantaCompileOptions()
{
    // Roblox's mutable globals: a field reached through one of these cannot be
    // turned into an import, because the table itself can be replaced.
    static const char* const kMutableGlobals[] = {
        "Game", "Workspace", "game", "plugin", "script", "shared", "workspace",
        "_G", "_ENV", nullptr,
    };

    Luau::CompileOptions options;
    // debugLevel 2, not 1. At debugLevel <= 1 the compiler drops locals whose
    // values are compile-time constants ("we don't need to allocate and assign
    // const locals, since their uses will be constant-folded"), and a closure
    // that captures one then captures a constant instead of an upvalue - so a
    // function that plainly has nine upvalues reports seven. Roblox's own
    // scripts keep theirs, and the debug library is expected to agree.
    options.debugLevel = 2;
    options.optimizationLevel = 1;
    options.mutableGlobals = kMutableGlobals;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";
    return options;
}
