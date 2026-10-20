//
//  decompile.hpp
//  vanta-base
//
//  The Luau decompiler, as a function call.
//
//  `decompile()` used to base64 a script's bytecode and POST it to
//  api.lua.expert. That meant every call was a network round trip: slow, dead
//  without a connection, and it handed a third party every script the user
//  looked at. The decompiler itself now ships inside libVanta.dylib as a static
//  archive built from `decompiler/` (a Rust workspace), and this header is the
//  whole of the C++ side of that boundary.
//
//  Nothing here runs on Roblox's thread. The Rust entry point spawns its own
//  thread with a large stack, catches every panic, and takes a deadline, so the
//  worst a malformed chunk can do is come back as an error string - see
//  decompiler/vanta-decompiler/src/lib.rs. The caller is still expected to be on
//  a worker (unc::Yield::start), because decompiling is seconds of CPU work and
//  the rule in async.hpp is that nothing which waits waits on the scheduler.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(VANTA_HAVE_DECOMPILER) && VANTA_HAVE_DECOMPILER
extern "C" {
    // Answers 1 when *out is source and 0 when it is the reason there is none.
    // *out is a NUL-terminated UTF-8 string owned by the callee.
    int vanta_decompile(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                       uint32_t timeoutMs, char** out);
    // As above, plus the script's own name. A ModuleScript's table is named
    // after the script and the bytecode records none of it, so it is passed in.
    int vanta_decompile_named(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                             uint32_t timeoutMs, const uint8_t* name, size_t nameLength,
                             char** out);
    void vanta_decompile_free(char* text);
}
#endif

namespace decompiler {

    // Roblox does not ship stock Luau opcodes: the client multiplies every
    // opcode by a constant on the way out, and 203 is the multiplier that
    // undoes it. Stock Luau bytecode uses 1, the identity.
    inline constexpr uint8_t kRobloxOpcodeKey = 203;

    // How long one decompile may take before it is abandoned. Generous - the
    // largest scripts in a real game are a second or two of work, and the
    // ceiling exists to bound a pathological chunk, not to hurry a big one.
    inline constexpr uint32_t kTimeoutMs = 30000;

    // Whether this build has the decompiler linked in. A build configured with
    // -DVANTA_DECOMPILER=off answers false and the Lua side falls back to the
    // remote API, so the function still exists either way.
    inline constexpr bool available()
    {
#if defined(VANTA_HAVE_DECOMPILER) && VANTA_HAVE_DECOMPILER
        return true;
#else
        return false;
#endif
    }

    // Decompiles a plain (already decompressed) Luau chunk - exactly the bytes
    // getscriptbytecode answers with. Answers whether `out` is source; when it
    // is not, `out` carries the reason.
    // `name` is the script's own name, used to name the table a ModuleScript
    // returns - `PlayerModule`, `BezierPath`. It may be empty; nothing else
    // depends on it.
    inline bool run(const std::string& bytecode, std::string& out,
                    const std::string& name = std::string())
    {
#if defined(VANTA_HAVE_DECOMPILER) && VANTA_HAVE_DECOMPILER
        char* text = nullptr;
        const int ok = vanta_decompile_named(
            reinterpret_cast<const uint8_t*>(bytecode.data()), bytecode.size(),
            kRobloxOpcodeKey, kTimeoutMs,
            name.empty() ? nullptr : reinterpret_cast<const uint8_t*>(name.data()),
            name.size(), &text);
        if (!text) {
            out = "the decompiler could not allocate its answer";
            return false;
        }
        out.assign(text);
        vanta_decompile_free(text);
        return ok != 0;
#else
        (void)bytecode;
        (void)name;
        out = "this build has no decompiler linked in";
        return false;
#endif
    }
}
