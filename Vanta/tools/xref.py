#!/usr/bin/env python3
"""Find RobloxPlayer code that references a string.

    tools/xref.py "ProximityPrompt_Triggered"

Roblox strips symbols, but it logs and asserts constantly, so most engine
functions can be reached from a string only they use. This finds the string in
the read-only data, scans __text for `lea reg, [rip+disp]` instructions whose
target is that string, and reports the function each one lives in (via
LC_FUNCTION_STARTS).
"""
import bisect, re, struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from funcs import load, BIN


def sections(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off, out = 32, []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                name = data[so:so + 16].rstrip(b"\0").decode()
                seg = data[so + 16:so + 32].rstrip(b"\0").decode()
                addr, size, fileoff = struct.unpack_from("<QQI", data, so + 32)
                out.append((seg, name, addr, size, fileoff))
                so += 80
        off += cmdsize
    return out


def main():
    needle = sys.argv[1].encode()
    data = open(BIN, "rb").read()
    secs = sections(data)

    text = next(s for s in secs if s[1] == "__text")
    text_lo, text_hi, text_off = text[2], text[2] + text[3], text[4]

    # every occurrence of the string in any read-only data section
    targets = []
    for seg, name, addr, size, fileoff in secs:
        if seg != "__TEXT" or name == "__text":
            continue
        blob = data[fileoff:fileoff + size]
        start = 0
        while True:
            i = blob.find(needle, start)
            if i < 0:
                break
            if i == 0 or blob[i - 1] == 0:      # string start, not a suffix
                targets.append(addr + i)
            start = i + 1
    if not targets:
        print("string not found")
        return
    print("string at: " + " ".join(hex(t) for t in targets))

    starts = load(BIN)
    code = data[text_off:text_off + text[3]]

    # 48/4c 8d <modrm with mod=00 rm=101> disp32  ->  lea reg, [rip+disp]
    hits = []
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code):
        i = m.start()
        if i + 7 > len(code):
            continue
        disp = struct.unpack_from("<i", code, i + 3)[0]
        target = text_lo + i + 7 + disp
        if target in targets:
            hits.append(text_lo + i)

    print(f"{len(hits)} reference(s):")
    for addr in hits:
        k = bisect.bisect_right(starts, addr) - 1
        owner = starts[k] if k >= 0 else 0
        print(f"  ref 0x{addr:X}  in function 0x{owner:X}  (+0x{addr - owner:X})")


if __name__ == "__main__":
    main()
