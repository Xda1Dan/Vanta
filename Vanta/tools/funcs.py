#!/usr/bin/env python3
"""List RobloxPlayer function entry points from LC_FUNCTION_STARTS.

    tools/funcs.py 0x102C62626 0x102C63500     # every entry in a range
    tools/funcs.py 0x102C62626 +12             # the next 12 entries

LC_FUNCTION_STARTS is a ULEB128 delta chain from the first __TEXT address, so
it names every function the linker emitted - including the static helpers that
never make it into the symbol table. Source order is address order within a
translation unit, which is what makes it usable for locating a function by its
position between two already-identified neighbours.
"""
import os, struct, sys

# The client has moved between Roblox.app and RobloxPlayer.app across builds.
DEFAULT_BIN = "/Applications/Roblox.app/Contents/MacOS/RobloxPlayer"
if not os.path.exists(DEFAULT_BIN):
    DEFAULT_BIN = "/Applications/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
BIN = os.environ.get("ROBLOX_BIN", DEFAULT_BIN)

def load(path):
    data = open(path, "rb").read()
    magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<8I", data, 0)
    assert magic == 0xfeedfacf, hex(magic)
    off = 32
    starts = None
    textbase = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off + 8:off + 24].rstrip(b"\0")
            vmaddr = struct.unpack_from("<Q", data, off + 24)[0]
            if segname == b"__TEXT":
                textbase = vmaddr
        elif cmd == 0x26:  # LC_FUNCTION_STARTS
            dataoff, datasize = struct.unpack_from("<2I", data, off + 8)
            starts = data[dataoff:dataoff + datasize]
        off += cmdsize

    out, addr, i = [], textbase, 0
    while i < len(starts):
        delta, shift = 0, 0
        while True:
            b = starts[i]; i += 1
            delta |= (b & 0x7f) << shift
            if not (b & 0x80):
                break
            shift += 7
        if delta == 0:
            break
        addr += delta
        out.append(addr)
    return out

if __name__ == "__main__":
    funcs = load(BIN)
    lo = int(sys.argv[1], 0)
    arg = sys.argv[2] if len(sys.argv) > 2 else "+20"
    idx = next(i for i, a in enumerate(funcs) if a >= lo)
    if arg.startswith("+"):
        sel = funcs[idx:idx + int(arg[1:])]
    else:
        hi = int(arg, 0)
        sel = [a for a in funcs[idx:] if a <= hi]
    for n, a in enumerate(sel):
        print(f"{n:4}  0x{a:X}  (+0x{a - lo:X})")
