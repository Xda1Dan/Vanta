//
//  luaobj.hpp
//  vanta-base
//
//  Roblox's Luau object layouts, as read out of the shipped binary rather than
//  assumed from src/luau. The vendored tree is Roblox's own fork - LUA_TINTEGER
//  sits at tag 4, which is why LUA_TFUNCTION is 8 and not 7 - and TValue,
//  Closure and TString match it closely. lua_State does not: Roblox reorders it
//  on every other build, so nothing here is taken on faith from the headers.
//
//  Every offset below was read out of a function whose identity is already
//  established, named in the comment beside it. tools/dis.sh reproduces them
//  (ROBLOX_BIN=/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer for
//  0.744). The 0.744 values below were re-measured from that binary and agree
//  with the Z4 dump's structs.hpp, whose enc:: field encodings are noted where
//  they apply.
//

#pragma once

#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace luaobj {

    // sizeof(TValue), from lua_gettop: (L->top - L->base) >> 4.
    inline constexpr int kValueSize = 16;

    // Type tags. Roblox's fork inserts LUA_TINTEGER at 4, shifting everything
    // above it up by one against upstream Luau. 0.744 keeps that order and adds
    // LUA_TBUFFER (11), LUA_TCLASS (12) and LUA_TOBJECT (13) past the thread,
    // which is what LUA_T_COUNT=14 counts to.
    inline constexpr int kTagNil = 0;
    inline constexpr int kTagBoolean = 1;
    inline constexpr int kTagLightUserdata = 2;
    inline constexpr int kTagNumber = 3;     // utils::pushNumber writes this for a double
    inline constexpr int kTagInteger = 4;    // the tag Roblox's fork inserts
    inline constexpr int kTagVector = 5;
    inline constexpr int kTagString = 6;     // lua_touserdata brackets it: 2 light, 9 full
    inline constexpr int kTagTable = 7;      // lua_setmetatable's tag-7 arm writes Table+32
    inline constexpr int kTagFunction = 8;   // lua_iscfunction: cmpl $8, 0xc(%rax)
    inline constexpr int kTagUserdata = 9;   // lua_touserdata's tag-9 arm reads +0x10
    inline constexpr int kTagThread = 10;
    inline constexpr int kTagBuffer = 11;
    inline constexpr int kTagClass = 12;
    inline constexpr int kTagObject = 13;

    namespace value {
        inline constexpr int gc = 0x0;   // the GCObject pointer
        inline constexpr int tt = 0xc;   // lua_iscfunction reads the tag here
    }

    // lua_State, 136 bytes on 0.744. Re-measured on 0.744 from lua_gettop
    // (top/base), the LuaVMLoad wrapper and lua_pushcclosurek (global),
    // lua_settop (ci) and the Z4 structs, which the disassembly agrees with
    // field for field. The 0.742 layout it replaces had ci at 0x8, global at
    // 0x18, top at 0x20 and base at 0x28 - every one of those moved.
    namespace state {
        inline constexpr int marked = 0x0;         // luaC_threadbarrier: testb $4, (%rdi)
        inline constexpr int memcat = 0x1;
        inline constexpr int tt = 0x2;             // luaF_newLclosure: movb $8, 0x2(%rax)
        inline constexpr int status = 0x03;        // see kStatusYield below
        inline constexpr int activememcat = 0x04;  // luaF_newLclosure copies it into the closure
        inline constexpr int singlestep = 0x05;
        inline constexpr int isactive = 0x06;
        inline constexpr int gt = 0x08;
        inline constexpr int base_ci = 0x18;
        inline constexpr int nCcalls = 0x20;
        inline constexpr int baseCcalls = 0x22;
        inline constexpr int userdata = 0x28;      // RobloxExtraSpace
        inline constexpr int finalizers = 0x30;
        inline constexpr int namecall = 0x38;
        inline constexpr int gclist = 0x40;
        inline constexpr int openupval = 0x48;
        inline constexpr int ci = 0x50;
        inline constexpr int base = 0x58;
        inline constexpr int stack = 0x60;
        inline constexpr int stack_last = 0x68;
        inline constexpr int top = 0x70;
        inline constexpr int global = 0x78;
        // Encrypted on 0.744 (Z4: LSTATE_STACKSIZE_ENC, vmval3 - stored XOR'd
        // with the field's own address). Nothing in src/ reads it; if one
        // ever does, go through encDecode below rather than a raw load.
        inline constexpr int stacksize = 0x80;
        inline constexpr int size_ci = 0x84;
    }

    // Roblox short-circuits method calls on userdata through
    // LOP_NAMECALLUDATA, which reads a per-tag direct handler out of the global
    // state and never consults the metatable. lua_registeruserdatadirectaccess
    // is what fills that in, and its body gives the whole layout. Re-measured
    // on 0.744 - same shape, same numbers:
    //
    //     movq 0x78(%r15), %rax              ; g = L->global
    //     movq 0x3db0(%rax,%rcx,8), %r14     ; g->udatamt[tag]
    //     leaq (%rcx,%rcx,8), %rcx           ; tag * 9
    //     leaq (%rax,%rcx,8), %rbx           ; g + tag * 72
    //     addq $0x920, %rbx                  ; g->udatadirect[tag]
    //
    // 72 bytes per entry matches the three TValues and three pointers of
    // lua_UdataDirectAccessData, which fixes the field offsets below. Both the
    // base and the stride came through 0.744 unchanged; the tag byte of the
    // Udata they inspect moved as part of the CommonHeader reshuffle (0x3 on
    // both, but the bytes around it moved - see the header note above).
    namespace udatadirect {
        inline constexpr int base = 0x920;     // from the global state
        inline constexpr int amt = 0x3db0;     // the per-tag method cache, 8 bytes each
        inline constexpr int stride = 0x48;
        inline constexpr int indextm = 0x00;
        inline constexpr int newindextm = 0x10;
        inline constexpr int namecalltm = 0x20;
        inline constexpr int index = 0x30;
        inline constexpr int newindex = 0x38;
        inline constexpr int namecall = 0x40;
    }

    // Udata: the tag sits in the byte after the header, and lua_touserdata
    // hands back the data pointer, which is the object plus 0x10. Unchanged on
    // 0.744 (Z4: tag 0x3, len 0x4, metatable 0x8 - that one now stored
    // encoded, UDATA_META_ENC - and data at 0x10; neither metatable nor len is
    // read raw anywhere in src/).
    namespace udata {
        inline constexpr int tag = 0x3;
        inline constexpr int data = 0x10;
    }

    // Proto on 0.744 is a different building: 216 bytes, the constant and code
    // arrays moved deeper into the struct, and eight pointers are stored
    // ENCODED (Z4 marks them vmval1-4 - each stored XOR'd with / offset by its
    // own address). The raw fields below are the only ones src/ reads:
    // k/code/p and the four sizes. The encoded ones are listed so nobody
    // guesses at them later; reads go through encDecode<T, OP>.
    //
    //     encoded (do not read raw):      source 0x08 (vmval1)
    //                                     upvalues 0x10 (vmval3)
    //                                     locvars 0x20 (vmval1)
    //                                     lineinfo 0x40 (vmval3)
    //                                     abslineinfo 0x58 (vmval2)
    //                                     debugname 0x60 (vmval1)
    //                                     debuginsn 0x68 (vmval4)
    //                                     typeinfo 0x80 (vmval3)
    namespace proto {
        inline constexpr int nups = 0x3;
        inline constexpr int is_vararg = 0x4;
        inline constexpr int maxstacksize = 0x5;
        inline constexpr int numparams = 0x6;
        inline constexpr int flags = 0x7;
        inline constexpr int codeentry = 0x28;
        inline constexpr int p = 0x30;           // Proto**
        inline constexpr int k = 0x48;           // TValue*
        inline constexpr int code = 0x50;        // Instruction*
        inline constexpr int execdata = 0x70;
        inline constexpr int sizeupvalues = 0x8c;
        inline constexpr int linedefined = 0x90;
        inline constexpr int sizek = 0x94;
        inline constexpr int sizelineinfo = 0xa0;
        inline constexpr int sizecode = 0xa4;
        inline constexpr int sizep = 0xa8;
        inline constexpr int sizelocvars = 0xac;
        inline constexpr int feedbackvec = 0xb0;
    }

    // An open upvalue is stored as a LUA_TUPVAL object whose value pointer sits
    // at +8; a closed one is the value itself. aux_upvalue does exactly this
    // test - cmpl $0x10, 0xc(%rax) - which is where the tag comes from.
    // Unchanged on 0.744 (Z4 UpVal: v at 0x8).
    inline constexpr int kTagUpval = 16;
    namespace upval {
        inline constexpr int value = 0x8;
    }

    // lua_namecallatom reads the atom at +6, the length at +0x14 and getstr()s
    // the payload at +0x18. Unchanged on 0.744 (Z4 TString: atomflag 0x4,
    // atom 0x6, next 0x8, hash 0x10 - that one now stored encoded,
    // TSTRING_HASH_ENC - len 0x14, data 0x18). Nothing in src/ reads hash.
    namespace tstring {
        inline constexpr int atom = 0x6;
        inline constexpr int length = 0x14;
        inline constexpr int data = 0x18;
    }

    // From lua_pushcclosurek's getcurrenv: ci->func, then the closure's env.
    // nresults and flags come from the lua_pcall-shaped function; nothing in
    // src/ reads either one, so they are recorded, not relied on.
    //
    // **The field ORDER is not stable across builds.** Only `func` is derived;
    // `base` and `top` are hand-held, and they have now gone stale three times
    // in a row, each time by colliding with `func`:
    //
    //              base    func    top
    //   0.737      0x00    0x10    0x08
    //   0.738      0x18    0x08    0x10     the order is reversed
    //   0.742      0x00    0x18    0x08
    //   0.744      0x00    0x08    0x18     func and top swap back, and a new
    //                                       Proto* `p` sits between them
    //
    // On 0.744 the Z4 dump's CallInfo - base 0x00, func 0x08, p 0x10, top
    // 0x18, savedpc 0x20, stride 0x30 as before - is taken as the layout. The
    // stride holding at 0x30 and the unchanged nresults/flags at 0x28/0x2c are
    // what makes it consistent with the 0.742 measurements on either side.
    // **Whenever `func` moves, re-measure the other two** - the cheap check is
    // __vanta_cibase, which reads L->ci, L->base and L->top inside ONE call
    // and reports each word's distance from base.
    namespace callinfo {
        inline constexpr int base = 0x00;
        inline constexpr int func = 0x08;
        inline constexpr int p = 0x10;           // Proto*, new on 0.744
        inline constexpr int top = 0x18;
        inline constexpr int savedpc = 0x20;
        inline constexpr int nresults = 0x28;
        inline constexpr int flags = 0x2c;
    }

    // A yield does not unwind the C frame it happened under: luau_execute
    // returns normally and the thread is left marked. Roblox's own protected
    // call tests exactly this, which is where both the offset and the two
    // constants come from.
    inline constexpr int kStatusYield = 1;
    inline constexpr int kStatusBreak = 6;

    // And a third, which cost a fortnight of "Cobalt corrupts itself".
    //
    // `performcall` (ldo.cpp) does NOT run the function when the caller is
    // yieldable and the call asked for a postponed re-entry:
    //
    //     if (preparereentry) L->status = SCHEDULED_REENTRY; else luau_execute(L);
    //
    // so the frame is set up, nothing executes, and the thread is left marked
    // 0x7f for `resume_continue` to pick up. `pcall` and `xpcall` ask for
    // exactly that - `luaD_callint(L, func, LUA_MULTRET, lua_isyieldable(L))` -
    // which means **any pcall inside a function reached through one of our
    // wrappers comes back this way**, because a wrapper built with a
    // continuation is what makes the thread yieldable in the first place.
    //
    // Luau's own test is `isyielded`, and it names all three:
    //
    //     #define isyielded(L) ((L)->status == LUA_YIELD ||
    //                           (L)->status == LUA_BREAK ||
    //                           (L)->status == SCHEDULED_REENTRY)
    //
    // Testing only two made newcclosureCall answer `lua_gettop` - results - for
    // a call that had not run. luau_poscall then copied whatever was on the
    // stack into the frame, and from there the thread executed a proto against
    // another proto's constants: garbage line numbers, namecalls for methods
    // the source never wrote ("attempt to call missing method 'tick' of
    // table"), and a client that dies within seconds. tests/nc.lua is two
    // seconds of `newcclosure(function() pcall(function() end) end)` and killed
    // the client every time.
    inline constexpr int kStatusScheduledReentry = 0x7f;


    // The C half of this union is where Roblox diverges most from the vendored
    // header, so it is spelled out field by field:
    //
    //   lua_clonefunction   reads nupvalues, proto and uprefs
    //   lua_tocfunction     reads isC and the C function
    //   lua_pushcclosurek   writes the C function, cont and upvals
    //
    // 0.744's C arm, straight off lua_pushcclosurek's writes:
    //
    //     movq %r15, 0x28(%rax)        ; f   - the C function, MOVED from 0x18
    //     leaq 0x20(%rax), %r15
    //     xorq %r15, %r12              ; cont stored as cont ^ (closure+0x20)
    //     movq %r12, 0x20(%rax)        ; cont
    //     movq %rax, 0x18(%r13)        ; debugname - took the slot f vacated
    //
    // and the upvalue copy loop lands at closure+0x30, same as before. Lua
    // closures keep proto at 0x18 and uprefs at 0x20 (the Z4 dump's l arm,
    // and what lua_clonefunction reads). nupvalues and stacksize swap bytes
    // against 0.742: 0.742 had nupvalues at 0x6, 0.744 has preload 0x4,
    // nupvalues 0x5, stacksize 0x6.
    namespace closure {
        inline constexpr int isC = 0x3;
        inline constexpr int preload = 0x4;
        inline constexpr int nupvalues = 0x5;
        inline constexpr int stacksize = 0x6;
        inline constexpr int gclist = 0x8;
        inline constexpr int env = 0x10;

        inline constexpr int proto = 0x18;    // Lua closures
        inline constexpr int uprefs = 0x20;   // Lua closures

        inline constexpr int debugname = 0x18; // C closures
        inline constexpr int cont = 0x20;      // C closures, self-relative
        inline constexpr int cfunction = 0x28;
        inline constexpr int upvals = 0x30;

        // How `cont` is stored, which is a property of the build rather than a
        // constant - and it has now changed on every single build. 0.735's
        // lua_pushcclosurek stores `slot + cont` and reads it back by
        // subtracting the slot; 0.736's stores `cont - slot` and reads it back
        // by adding; 0.737's stores `cont ^ slot` and reads it back by XOR-ing;
        // 0.738's stores `slot - cont` and reads it back by subtracting the
        // stored value FROM the slot; 0.742 kept 0.738's shape.
        //
        // 0.744 is back to XOR - `xorq %r15, %r12` above, and the Z4 dump's
        // CLOSURE_CONT_ENC is vmval3, whose decode is `address ^ stored`. That
        // is the same arithmetic 0.737 had, so it reuses cont_op 2. Assuming
        // the wrong one writes a continuation the engine decodes to a wild
        // pointer, and jumps to it the moment a hooked C function resumes.
        // That took the client down inside sUNC's hookfunction check with every
        // offset around it correct, which is what made it hard to see: nothing
        // was pointing at the wrong field, the field was being written the
        // wrong way round.
        //
        // This was a boolean until 0.737, which it could not describe - and it
        // answered "added" for it by default, which is the failure above. It is
        // four-valued now: 0 = cont - slot (0.736), 1 = slot + cont (0.735),
        // 2 = cont ^ slot (0.737, 0.744), 3 = slot - cont (0.738), keeping the
        // numbering the boolean had. The dumper reads it off the instructions
        // that produce the stored value, operand order included:
        //     1 added        add rax, <cont>  ; add rax, <off> ; mov [c+off], rax
        //     0 cont - slot  add rax, <off>   ; sub <cont>, rax ; mov [c+off], <cont>
        //     2 xor          lea rax, [c+off] ; xor <cont>, rax ; mov [c+off], <cont>
        //     3 slot - cont  lea r13, [c+off] ; mov rax, r13 ; sub <cont>, rax
        //                    ; mov [c+off], rax
        inline constexpr int cont_op = 0x2;
    }

    // The Z4 dump's enc::value<T, OP> - how Roblox hides pointer-sized fields
    // in place on 0.744 (global_State::ptrencactive gates the whole scheme).
    // OP 1 stores `value + slot`, 2 `slot - value`, 3 `value ^ slot`, 4
    // `value - slot`, where slot is the field's own address. The two helpers
    // below are the read side; for writes prefer writing through the engine
    // (the way newcclosure does) or extend these with their inverses.
    template <int OP>
    inline uintptr_t encDecode(uintptr_t slot, uint64_t stored)
    {
        if constexpr (OP == 1) return static_cast<uintptr_t>(stored - slot);
        else if constexpr (OP == 2) return static_cast<uintptr_t>(slot - stored);
        else if constexpr (OP == 3) return static_cast<uintptr_t>(slot ^ stored);
        else return static_cast<uintptr_t>(stored + slot);
    }

    // A read that reports failure instead of faulting.
    //
    // `readable` below asks mincore whether a page is resident, which says
    // nothing about its protection: a PROT_NONE guard page is resident and
    // reading it kills the process. When the address came from memory another
    // thread is free to rewrite - a job list being rebuilt as a DataModel
    // loads, a struct being probed - the kernel has to do the copy so that a
    // bad address is an error code rather than a signal. sourcessss guards its
    // own job walk the Windows way, with VirtualQuery; this is that.
    inline bool safeRead(void* destination, uintptr_t source, size_t length)
    {
        if (!source || !length || !destination)
            return false;
        mach_vm_size_t copied = 0;
        const kern_return_t status = mach_vm_read_overwrite(
            mach_task_self(), static_cast<mach_vm_address_t>(source),
            static_cast<mach_vm_size_t>(length),
            reinterpret_cast<mach_vm_address_t>(destination), &copied);
        return status == KERN_SUCCESS && copied == length;
    }

    // The write half, and it exists for the same reason. `safeRead` succeeding
    // says the address is *readable*, which is not the same as writable: a
    // page mapped read-only reads fine and faults the moment it is stored to.
    // The one caller that writes to an address a script supplied
    // (`__vanta_poke`, which is how a candidate flag is found - see the
    // require-bypass memory) did read-then-store, so a read-only address was a
    // crash rather than a nil. The kernel does the copy here too, so it comes
    // back as a status.
    inline bool safeWrite(uintptr_t destination, const void* source, size_t length)
    {
        if (!destination || !length || !source)
            return false;
        const kern_return_t status = mach_vm_write(
            mach_task_self(), static_cast<mach_vm_address_t>(destination),
            reinterpret_cast<vm_offset_t>(source), static_cast<mach_msg_type_number_t>(length));
        return status == KERN_SUCCESS;
    }

    // safeRead for one value, answering a default when the address is not
    // readable.
    template <typename T>
    inline T safeValue(uintptr_t source, T fallback = T{})
    {
        T value{};
        return safeRead(&value, source, sizeof(T)) ? value : fallback;
    }

    // Whether a range can be read without faulting. Probing a struct layout
    // means following words that may not be pointers at all, and a wrong guess
    // is a segfault inside Roblox - mincore answers for the mapping first.
    inline bool readable(const void* address, size_t length)
    {
        if (!address)
            return false;
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        static const size_t pageSize = static_cast<size_t>(getpagesize());
        const uintptr_t first = start & ~(pageSize - 1);
        const uintptr_t last = (start + length - 1) & ~(pageSize - 1);

        char resident[64];
        const size_t pages = (last - first) / pageSize + 1;
        if (pages > sizeof(resident))
            return false;
        return mincore(reinterpret_cast<void*>(first), pages * pageSize, resident) == 0;
    }

    template <typename T>
    inline T read(const void* object, int offset)
    {
        T value;
        std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(T));
        return value;
    }

    inline uint8_t byte(const void* object, int offset)
    {
        return *(static_cast<const uint8_t*>(object) + offset);
    }

    template <typename T>
    inline void write(void* object, int offset, const T& value)
    {
        std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(T));
    }

    inline void writeByte(void* object, int offset, uint8_t value)
    {
        *(static_cast<uint8_t*>(object) + offset) = value;
    }

    // Copies one TValue without a write barrier, the way setobj2s does for
    // stack slots. Anything written into a live object this way has to be kept
    // reachable some other way - hookfunction anchors it in the registry.
    inline void copyValue(void* destination, int destinationOffset, const void* source, int sourceOffset)
    {
        std::memcpy(static_cast<uint8_t*>(destination) + destinationOffset,
                    static_cast<const uint8_t*>(source) + sourceOffset,
                    kValueSize);
    }

    inline bool yielded(uint64_t state)
    {
        const uint8_t status = byte(reinterpret_cast<void*>(state), state::status);
        return status == kStatusYield || status == kStatusBreak ||
               status == kStatusScheduledReentry;
    }

    // A continuation is stored relative to its own slot, so that a null one
    // reads back as the slot's address and decodes to zero. Which arithmetic
    // the engine uses is closure::cont_op, and the reasoning is there. The two
    // halves below must stay exact inverses of each other.
    inline uintptr_t decodeContinuation(const void* closure)
    {
        const uintptr_t slot = reinterpret_cast<uintptr_t>(closure) + closure::cont;
        const uintptr_t stored = read<uintptr_t>(closure, closure::cont);
        switch (closure::cont_op) {
        case 1:  return stored - slot;     // stored as slot + cont
        case 2:  return stored ^ slot;     // stored as cont ^ slot
        case 3:  return slot - stored;     // stored as slot - cont
        default: return stored + slot;     // stored as cont - slot
        }
    }

    inline void writeContinuation(void* closure, uintptr_t continuation)
    {
        const uintptr_t slot = reinterpret_cast<uintptr_t>(closure) + closure::cont;
        const uintptr_t stored = closure::cont_op == 1 ? slot + continuation
                               : closure::cont_op == 2 ? (continuation ^ slot)
                               : closure::cont_op == 3 ? slot - continuation
                                                       : continuation - slot;
        write<uintptr_t>(closure, closure::cont, stored);
    }
}
