#!/usr/bin/env python3
"""Find every direct call/jmp to an address in RobloxPlayer's __text.

    tools/callers.py 0x1053DBD68

Roblox is stripped, so the only way to name a function is by what reaches it.
This scans __text for E8/E9 rel32 whose target is the address given, and
reports the function each call site lives in (via LC_FUNCTION_STARTS).
"""
import bisect, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from funcs import load, BIN

def text_section(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == 0x19:
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                sect = data[so:so + 16].rstrip(b"\0")
                seg = data[so + 16:so + 32].rstrip(b"\0")
                addr, size = struct.unpack_from("<2Q", data, so + 32)
                fileoff = struct.unpack_from("<I", data, so + 48)[0]
                if seg == b"__TEXT" and sect == b"__text":
                    return addr, size, fileoff
                so += 80
        off += cmdsize
    raise SystemExit("no __text")

data = open(BIN, "rb").read()
target = int(sys.argv[1], 0)
addr, size, fileoff = text_section(data)
funcs = load(BIN)

hits = []
body = data[fileoff:fileoff + size]
for i in range(len(body) - 5):
    op = body[i]
    if op not in (0xE8, 0xE9):
        continue
    rel = struct.unpack_from("<i", body, i + 1)[0]
    site = addr + i
    if site + 5 + rel == target:
        j = bisect.bisect_right(funcs, site) - 1
        hits.append((site, funcs[j] if j >= 0 else 0, "call" if op == 0xE8 else "jmp"))

print(f"{len(hits)} direct reference(s) to {target:#x}")
for site, fn, kind in hits[:60]:
    print(f"  {kind} at {site:#x}  in function {fn:#x}  (+{site - fn:#x})")

# also lea rip-relative (vtable/pointer taking)
leas = []
for i in range(len(body) - 7):
    if body[i] == 0x48 and body[i + 1] == 0x8D:
        modrm = body[i + 2]
        if (modrm & 0xC7) == 0x05:
            rel = struct.unpack_from("<i", body, i + 3)[0]
            site = addr + i
            if site + 7 + rel == target:
                j = bisect.bisect_right(funcs, site) - 1
                leas.append((site, funcs[j] if j >= 0 else 0))
print(f"{len(leas)} lea reference(s)")
for site, fn in leas[:40]:
    print(f"  lea at {site:#x}  in function {fn:#x}  (+{site - fn:#x})")
