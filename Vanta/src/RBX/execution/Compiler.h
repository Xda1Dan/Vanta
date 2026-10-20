//
//  compiler.hpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "luau/Compiler/include/Luau/Compiler.h"
#include "luau/Bytecode/include/Luau/BytecodeBuilder.h"
#include "luau/Common/include/Luau/BytecodeUtils.h"
#include "luau/VM/include/lua.h"

#include "CompileOptions.h"
#include "Offsets.h"
#include "ClientAPI.h"

using BYTE = uint8_t;

class BytecodeEncoder : public Luau::BytecodeEncoder {
public:
    void encode(uint32_t* data, size_t count) override
    {
        for (size_t i = 0; i < count;) {
            uint8_t opcode = LUAU_INSN_OP(data[i]);
            const auto opLen = Luau::getOpLength(LuauOpcode(opcode));

            // LOP_CAPTURE's C operand is unused in upstream Luau and Roblox
            // fills it in anyway, which makes it a tripwire: the interpreter
            // checks it and marks the VM when it is empty.
            //
            // The vendored compiler beside this file is Roblox's fork, not
            // upstream, so it already writes it - `Compiler.cpp:1521` emits
            // `c.type == LCT_REF ? 0xff : 0`. Measured in a live game both
            // ways: with this normalisation and without it, the bit stays set.
            // It is here so that the executor does not depend on a line in
            // vendored code that an upstream sync would quietly drop, which is
            // exactly the sort of loss nothing would report.
            //
            //     0x102c9d3b5  jne  .LCT_VAL           ; A != LCT_REF
            //     0x102c9d3b7  cmp  esi, 0xffffff      ; the whole instruction
            //     0x102c9d3bd  jbe  0x102c9d429        ; C == 0
            //     ...
            //     0x102c9d429  mov  rax, [r11 + 0x70]  ; L->global
            //     0x102c9d42d  and  dword [rax+0x4c8], 0xefffffff
            //
            // 0x4c8 is `registry_free`, whose low 28 bits are the ref free list
            // and whose top bits are detection flags; clearing bit 28 is what
            // the stats ping item then reports (see gameldr.hpp for the sibling
            // check and for the accessor that gathers all of them). The same
            // three instructions appear in the second copy of the interpreter
            // at 0x102ca3c95.
            //
            // What Roblox's own compiler writes there is not a guess: reading
            // 188,697 chunks of real bytecode off this build - every script
            // `dump_bytecode.lua` could reach in three games - the C operand of
            // LOP_CAPTURE is **0xFF on all 118,295 LCT_REF captures** and 0 on
            // every one of the 1,941,340 LCT_VAL and LCT_UPVAL captures. So the
            // rule is exact, and this writes what their compiler writes.
            if (opcode == LOP_CAPTURE && LUAU_INSN_A(data[i]) == LCT_REF)
                data[i] = (data[i] & 0x00FFFFFFu) | (0xFFu << 24);

            data[i] = uint8_t(opcode * 227) | (data[i] & ~0xff);
            i += opLen;
        }
    }
};

inline std::string compileScript(const std::string& source)
{
    BytecodeEncoder bytecodeEncoder;
    // The options moved to compileopts.hpp when the editor's checker started
    // compiling too: what it calls an error has to be what this refuses.
    return Luau::compile(source, vantaCompileOptions(), {}, &bytecodeEncoder);
}
