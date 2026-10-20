//
//  signature.hpp
//  vanta-base
//
//  Roblox refuses to parse a chunk whose bytecode signature does not check out.
//  Ours is compiled in-process and never signed, so the executor opts its own
//  chunks out and leaves the check in place for everything else.
//

#pragma once

#include <cstddef>
#include <cstdint>

// luau_load hands every chunk to this before parsing a single byte of it, and
// answers "bytecode corrupted" unless it comes back 0. The check only starts
// looking for a signature footer once the chunk is big enough to hold one:
//
//     10143bc32  mov  rdx, qword ptr [rsi + 16]   ; bytecode size
//     10143bc36  mov  r12d, 1                     ; "unsigned", accepted
//     10143bc3c  cmp  rdx, 280
//     10143bc43  jb   .return
//
// Under 280 bytes a chunk is waved through as unsigned; at 280 and above the
// last 24 bytes are read back as a signature that was never appended, and the
// garbage that comes out fails. That boundary is the whole bug: a two-line
// script compiles small enough to slip under it, and one line of Lua more can
// be enough to cross it, which is why adding a single character to a script
// turns it from working into "bytecode corrupted".
//
// Returning 0 unconditionally would also stop the game verifying its own
// scripts, so the hook only answers for the buffer this thread is loading
// right now and defers to Roblox for every other chunk.

namespace signature {

    // (lua_State*, chunkname, bytecode, size) -> 0 ok, 1 corrupted. This is the
    // gate's real signature: the call site at 0x1029aa242 passes four
    // arguments, not upstream's five with an env.
    using CheckFn = int (*)(uint64_t, const char*, const char*, size_t);

    inline CheckFn original = nullptr;

    // The bytecode this thread has in flight, or null between loads.
    inline thread_local const char* inflight = nullptr;

    inline int check(uint64_t state, const char* chunkname, const char* bytecode, size_t size)
    {
        if (bytecode && bytecode == inflight)
            return 0;
        return original ? original(state, chunkname, bytecode, size) : 0;
    }

    // Claims a buffer as ours for as long as this stays on the stack. luau_load
    // passes the pointer straight through, so the hook can match on it exactly.
    //
    // Half of what has to be true during a load of ours, so it is not what call
    // sites construct: `load::Ours` in loadguard.hpp holds this and the load
    // clock together.
    //
    // 0x1014BB465 is the SigGuard gate *and* the 280-byte check - the research
    // notes name the same function both ways. It used to be hooked twice: once
    // through the rebased entry and once through an `offsets::sigguard` that
    // had lost the 0x100000000 image base. The second hook never installed
    // (verify::isFunctionEntry rejects an address below the image), and if it
    // had it would have wrapped this one. There is one hook, here.

    class Claim {
    public:
        explicit Claim(const char* bytecode) : previous(inflight) { inflight = bytecode; }
        ~Claim() { inflight = previous; }

        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;

    private:
        const char* previous;
    };
}
