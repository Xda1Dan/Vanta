//
//  vulnpatch.hpp
//  vanta-base
//
//  Blocking a specific engine member for executor threads only, through the
//  member's own reflection descriptor rather than an __index / __namecall hook.
//
//  Every callable member Roblox exposes to Lua is a `Reflection::*Descriptor`
//  object whose vtable holds an "invoke" slot the reflection bridge calls to run
//  it: `(*descriptor)[slot](descriptor, instance, L, nargs)` for the synchronous
//  families, and a marshalled variant for the yielding one. To block a member we
//  clone its descriptor's vtable, point the invoke slot at one shared trampoline,
//  and stash the real invoke inside the clone. The trampoline finds the calling
//  thread among the invoke's arguments (any real lua_State in this VM answers
//  `*(L+0x30) == global_State`), and if that thread carries the executor mark it
//  raises; otherwise it tail-calls the original invoke unchanged. So the engine's
//  own callers (core scripts, the game) are untouched and only our threads are
//  refused - which is exactly what every reference executor does, but per
//  descriptor instead of through a central metamethod. The mark is the same
//  capability bit checkcaller reads (executor.hpp, kExecutorMark = 0x40).
//
//  Nothing here trusts a reference offset: the invoke slot per family and the
//  `(Descriptor*, Instance*, L*)` argument order were read out of this build's
//  own descriptor vtables (BoundFuncDesc vf[3], BoundYieldFuncDesc vf[2]), and
//  the lua_State fields (extra space +0x78, global +0x30) are the ones already
//  derived in luau-object-layouts.
//
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Offsets.h"
#include "ClientAPI.h"
#include "LuaObject.h"
#include "Executor.h"
#include "Journal.h"

namespace vulnpatch {

// The global_State shared by every thread in the VM we registered into. A real
// lua_State among the invoke's arguments is recognised by matching this; it is
// how the trampoline tells the calling thread apart from the other pointers the
// invoke is handed. Set once, from the first descriptor install.
inline uint64_t knownGlobal = 0;

// Where a lua_State keeps its global_State pointer - the field that identifies a
// real thread of this VM.
//
// This MUST come from luaobj, never a literal. It was written here as 0x30, which
// was right for 0.735 and is `state::base` on 0.736: every Luau struct offset
// moves between builds, and the offset dumper rewrites luaobj.hpp for the build
// in hand. With the literal, asStateInVM compared two threads' stack bases
// instead of their global_State, findCallingThread never identified the caller,
// and vanta_vp_check took its "thread not identified - never block" exit. The
// whole blocklist installed cleanly (2652 descriptors, no failures) and then let
// every blocked call through - a security hole that reads exactly like a working
// build from the outside. Aliasing it means the next build move fixes this file
// along with everything else.
constexpr int kStateGlobal = luaobj::state::global;

// Safe mode. The blocklist is a user switch now: the GUI writes it to
// ~/Documents/Vanta/.vanta-settings and main.cpp applies it on every join.
//
// It gates the *check*, not the install, because an install cannot be taken
// back. Descriptors are process-global and `patched` remembers them, so a
// player who joins with safe mode on and then teleports into a place with it
// off is still running through vtables cloned on the first join - skipping the
// blocklist script on that second join would change nothing. This flag is the
// one place that is true for every descriptor, whenever it was patched.
inline bool enabled = true;

// Where the original invoke is kept inside a cloned vtable, as a byte offset
// from the descriptor's vptr. Past every real slot these descriptors use (their
// primary vtables are a handful of entries), so it never overlaps one the engine
// reads. The trampoline tail-jumps through it.
constexpr int kStashOff = 0x120;

// A candidate pointer, treated as a lua_State, is real iff its global_State
// field matches the VM's. Fully guarded - a wrong guess reads nothing.
inline uint64_t asStateInVM(uint64_t candidate)
{
    if (candidate < 0x10000 || !knownGlobal)
        return 0;
    uint64_t global = 0;
    if (luaobj::safeRead(&global, candidate + kStateGlobal, sizeof(global)) && global == knownGlobal)
        return candidate;
    return 0;
}

// The calling thread, found among the invoke's six integer-argument registers.
//
// The synchronous families (BoundFuncDesc/CustomBoundFuncDesc) pass L straight
// through as an argument register, so the first loop finds it at once. The
// yielding family marshals its call into a small bundle and hands L inside it, so
// the register itself is not L - it is one or two dereferences away, in the
// shared_ptr the invoke is given or the argument object beside it. The second and
// third loops walk those, at the handful of offsets a std::string/shared_ptr/
// object header spans.
//
// Every read is guarded (a wrong pointer reads nothing rather than faulting) and
// every candidate is confirmed by matching the VM's global_State - a field only a
// real lua_State in this VM carries - so the wider net cannot be fooled into
// returning a pointer that is not a thread, and never returns a thread from
// another VM. The cost is paid only on a call to a blocked member (which is about
// to error anyway); the common synchronous case exits on the first probe.
inline uint64_t findCallingThread(const uint64_t regs[6])
{
    static const int kOffsets[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};

    // Direct: L is one of the argument registers.
    for (int i = 0; i < 6; i++)
        if (uint64_t s = asStateInVM(regs[i]))
            return s;

    // One dereference: L sits inside an object the invoke was handed.
    for (int i = 0; i < 6; i++)
        for (int off : kOffsets) {
            uint64_t p = 0;
            if (luaobj::safeRead(&p, regs[i] + off, sizeof(p)))
                if (uint64_t s = asStateInVM(p))
                    return s;
        }

    // Two dereferences: through a shared_ptr's pointee to the thread it holds.
    for (int i = 0; i < 6; i++) {
        uint64_t inner = 0;
        if (!luaobj::safeRead(&inner, regs[i], sizeof(inner)) || inner < 0x10000)
            continue;
        for (int off : kOffsets) {
            uint64_t p = 0;
            if (luaobj::safeRead(&p, inner + off, sizeof(p)))
                if (uint64_t s = asStateInVM(p))
                    return s;
        }
    }
    return 0;
}

}  // namespace vulnpatch

// ---- C-callable pieces the assembly trampoline reaches ----
//
// extern "C" so the trampoline can name them without mangling; UNC.hpp is the
// only translation unit that includes this header, so there is no ODR clash.

// Returns the calling thread when it is one of ours and must be refused, or 0 to
// let the call through. Called with the invoke's own six argument registers.
extern "C" uint64_t vanta_vp_check(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5)
{
    if (!vulnpatch::enabled)
        return 0;                     // safe mode off - everything passes through
    const uint64_t regs[6] = {a0, a1, a2, a3, a4, a5};
    const uint64_t L = vulnpatch::findCallingThread(regs);
    if (!L)
        return 0;                     // thread not identified - never block the engine
    if (!isExecutorThread(L))
        return 0;                     // a core/game thread - the engine's own use
    return L;                         // ours: refuse it
}

// Raises out of the blocked call. lua_error longjmps to the nearest protected
// boundary and never returns - the same unwind a member's own luaL_error would
// take, from one frame shallower.
extern "C" void vanta_vp_block(uint64_t L)
{
    static const char message[] = "attempt to call a blocked function";
    rbx::lua_pushlstring(L, message, sizeof(message) - 1);
    rbx::lua_error(L);
    for (;;) {}  // unreachable; keeps the naked caller's tail-jump well-formed
}

// One trampoline for every descriptor family. The reflection bridge always calls
// the invoke as `slot(descriptor /*rdi*/, ...)`, so the descriptor - whose vtable
// we replaced - is rdi, and the stashed original invoke is at kStashOff from its
// vptr. The six integer registers are saved across the check (the real Lua
// arguments live on the Lua stack, so nothing in the float or stack-argument area
// needs preserving) and restored before either tail-jump.
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _vanta_vp_tramp\n"
    "_vanta_vp_tramp:\n"
    "    push %rbp\n"
    "    mov  %rsp, %rbp\n"
    "    sub  $0x40, %rsp\n"
    "    mov  %rdi, 0x00(%rsp)\n"
    "    mov  %rsi, 0x08(%rsp)\n"
    "    mov  %rdx, 0x10(%rsp)\n"
    "    mov  %rcx, 0x18(%rsp)\n"
    "    mov  %r8,  0x20(%rsp)\n"
    "    mov  %r9,  0x28(%rsp)\n"
    "    call _vanta_vp_check\n"       // args already in rdi..r9
    "    mov  %rax, %r10\n"           // r10 = thread to block, or 0
    "    mov  0x00(%rsp), %rdi\n"
    "    mov  0x08(%rsp), %rsi\n"
    "    mov  0x10(%rsp), %rdx\n"
    "    mov  0x18(%rsp), %rcx\n"
    "    mov  0x20(%rsp), %r8\n"
    "    mov  0x28(%rsp), %r9\n"
    "    add  $0x40, %rsp\n"
    "    pop  %rbp\n"
    "    test %r10, %r10\n"
    "    jne  1f\n"
    "    mov  (%rdi), %rax\n"         // rdi = descriptor -> our clone vtable
    "    jmp  *0x120(%rax)\n"         // original invoke (kStashOff)
    "1:\n"
    "    mov  %r10, %rdi\n"           // the calling thread
    "    jmp  _vanta_vp_block\n");

extern "C" void vanta_vp_tramp();

namespace vulnpatch {

namespace layout = offsets::structs;

// Whether an address is inside Roblox's image. A copy of Probe::inImage kept here
// so this header does not depend on UNC.hpp (which depends on it).
inline bool Probe_inImage(uintptr_t address)
{
    const uintptr_t base = 0x100000000 + hostImage().slide;
    return address >= base && address < base + 0x10000000;
}

// Descriptors are global and live for the process, so a clone is made once and
// kept forever; the set stops a second install (on a teleport re-run) from
// cloning a descriptor whose vtable is already ours.
inline std::vector<std::vector<uint64_t>*> clones;
inline std::unordered_set<uintptr_t> patched;

// Reads a C string (a std::type_info name), guarded.
//
// One read for the whole buffer first. Every byte used to be its own
// mach_vm_read_overwrite, so a mangled descriptor class name of 60-160
// characters cost that many syscalls per member, and that was most of the ~60 ms
// first-join blocklist pass (VULNTIME, 2026-09-03). The bulk read fails where the
// name sits within `cap` bytes of an unmapped page, so the byte walk stays as the
// fallback for exactly that case. Either way `out` is NUL-terminated.
inline bool readTypeName(uintptr_t at, char* out, size_t cap)
{
    if (luaobj::safeRead(out, at, cap - 1)) {
        out[cap - 1] = '\0';
        return true;
    }
    for (size_t i = 0; i < cap - 1; i++) {
        uint8_t byte = 0;
        if (!luaobj::safeRead(&byte, at + i, 1))
            return false;
        out[i] = static_cast<char>(byte);
        if (!byte)
            return true;
    }
    out[cap - 1] = '\0';
    return true;
}

// The invoke slot for a descriptor, chosen from its most-derived class name, or
// -1 if it is not a callable-member descriptor we know how to block. Read from
// this build: BoundFuncDesc / CustomBoundFuncDesc keep the invoke at vf[3] with
// the clean (Descriptor*, Instance*, L, nargs) shape; BoundYieldFuncDesc and
// BoundAsyncCallbackDesc keep it at vf[2]. The check order matters - the yield,
// async and custom names all contain the sync name as a substring.
inline int invokeSlotFor(uintptr_t descriptor, std::string& familyOut)
{
    uint64_t vptr = 0, typeInfo = 0, namePtr = 0;
    if (!luaobj::safeRead(&vptr, descriptor, sizeof(vptr)) || !vptr ||
        !luaobj::safeRead(&typeInfo, vptr - 8, sizeof(typeInfo)) || !typeInfo ||
        !luaobj::safeRead(&namePtr, typeInfo + 8, sizeof(namePtr)) || !namePtr)
        return -1;

    char name[160];
    if (!readTypeName(namePtr, name, sizeof(name)))
        return -1;
    const std::string_view n(name);

    auto has = [&](const char* needle) { return n.find(needle) != std::string_view::npos; };

    if (has("BoundYieldFuncDesc") || has("YieldFunctionDescriptor")) {
        familyOut = "yield";
        return 0x10;
    }
    if (has("BoundAsyncCallbackDesc")) {
        familyOut = "async";
        return 0x10;
    }
    if (has("CustomBoundFuncDesc")) {
        familyOut = "custom";
        return 0x18;
    }
    if (has("BoundFuncDesc") || has("FunctionDescriptor")) {
        familyOut = "sync";
        return 0x18;
    }
    return -1;
}

// Clone the descriptor's vtable, point its invoke slot at the trampoline, and
// keep the real invoke at kStashOff so the trampoline can pass through to it.
// Returns false (and logs why) when the descriptor is not one we can block.
inline bool install(uintptr_t descriptor, uint64_t vmGlobal, const char* who)
{
    if (!descriptor)
        return false;
    if (patched.count(descriptor))
        return true;

    if (!knownGlobal)
        knownGlobal = vmGlobal;

    std::string family;
    const int slot = invokeSlotFor(descriptor, family);
    if (slot < 0) {
        journal::writef("VULN skip %s - not a callable descriptor", who);
        return false;
    }

    uint64_t vptr = 0;
    if (!luaobj::safeRead(&vptr, descriptor, sizeof(vptr)) || !vptr)
        return false;

    // slots[0] is the offset-to-top two qwords below the vptr, so the vptr the
    // engine sees is &slots[2]. Copy the header plus enough of the primary
    // vtable to cover anything read through it; the stash sits past that.
    auto* clone = new std::vector<uint64_t>(48, 0);
    if (!luaobj::safeRead(clone->data(), vptr - 0x10, sizeof(uint64_t) * 24)) {
        delete clone;
        return false;
    }

    uint64_t* newVptr = clone->data() + 2;
    const uint64_t original = newVptr[slot / 8];
    if (!Probe_inImage(original)) {
        // The slot we would replace does not point into Roblox - the family
        // detection or the slot is wrong for this descriptor. Leave it alone.
        journal::writef("VULN skip %s - invoke slot %#x not in image (%llx)", who, slot,
                        (unsigned long long)original);
        delete clone;
        return false;
    }

    newVptr[kStashOff / 8] = original;                        // original invoke, for passthrough
    newVptr[slot / 8] = reinterpret_cast<uint64_t>(&vanta_vp_tramp);

    const uint64_t newVptrValue = reinterpret_cast<uint64_t>(newVptr);
    if (!luaobj::safeWrite(descriptor, &newVptrValue, sizeof(newVptrValue))) {
        journal::writef("VULN FAIL %s - could not rewrite descriptor vptr", who);
        delete clone;
        return false;
    }

    clones.push_back(clone);
    patched.insert(descriptor);
    journal::writef("VULN block %s (%s, slot %#x)", who, family.c_str(), slot);
    return true;
}

}  // namespace vulnpatch
