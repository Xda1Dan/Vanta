#!/usr/bin/env python3
"""Find every rip-relative reference in RobloxPlayer's __text to a data address.

    tools/datarefs.py 0x10753EB28                 # one address
    tools/datarefs.py 0x10753EAE0 0x10753EB60     # an inclusive range

`callers.py` finds control flow and `xref.py` finds string literals; neither
finds who *reads or writes a global*. A stripped Roblox global has no name, so
the only way to learn what a flag, counter or cached pointer is for is to list
every instruction that touches it - which is what this does.

x86-64 addresses globals as `[rip + disp32]`: a ModRM byte with mod=00 rm=101
followed by a signed 32-bit displacement, resolved against the address of the
*next* instruction. The instruction may carry 0-4 bytes of trailing immediate
after that displacement, so a byte scan alone cannot know where it ends and
will invent references that are not there. Every candidate is therefore
re-disassembled with capstone from the start of its containing function
(LC_FUNCTION_STARTS gives exact boundaries, so a linear sweep from there is
correct) and kept only if a real instruction claims it. A whole-image pass is
about 40s; -q skips the verify step when only a count is wanted.
"""
import bisect, struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from funcs import load, BIN


def text_section(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
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


def scan(code, base, lo, hi):
    """Byte-level candidate scan: positions whose disp32 could reach [lo,hi].

    Walked once per 4-byte alignment so struct.iter_unpack does the decoding in
    C; a per-byte Python loop over 88MB is an order of magnitude slower.
    """
    cands = []
    for k in range(4):
        m = (len(code) - k) // 4 * 4
        for i, (disp,) in enumerate(struct.iter_unpack("<i", code[k:k + m])):
            p = k + 4 * i
            end = base + p + 4 + disp          # end-of-insn if 0 trailing bytes
            if lo - 4 <= end <= hi and p and (code[p - 1] & 0xC7) == 0x05:
                cands.append(p)
    return sorted(cands)


def verify(cands, code, base, lo, hi, funcs):
    """Keep only candidates a real decoded instruction accounts for."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    seen, hits = set(), []
    for p in cands:
        site = base + p
        j = bisect.bisect_right(funcs, site) - 1
        fn = funcs[j] if j >= 0 else base
        if fn in seen:
            continue
        seen.add(fn)
        j2 = bisect.bisect_right(funcs, fn)
        end = min(funcs[j2] if j2 < len(funcs) else base + len(code), fn + 0x8000)
        off = fn - base
        for insn in md.disasm(code[off:end - base], fn):
            for op in insn.operands:
                if op.type != capstone.x86.X86_OP_MEM:
                    continue
                if op.mem.base != capstone.x86.X86_REG_RIP:
                    continue
                t = insn.address + insn.size + op.mem.disp
                if lo <= t <= hi:
                    hits.append((insn.address, fn, t, "%s %s" % (insn.mnemonic, insn.op_str)))
    return sorted(hits)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    quiet = "-q" in sys.argv
    lo = int(args[0], 0)
    hi = int(args[1], 0) if len(args) > 1 else lo
    data = open(BIN, "rb").read()
    base, size, fileoff = text_section(data)
    code = data[fileoff:fileoff + size]

    cands = scan(code, base, lo, hi)
    if quiet:
        print(f"{len(cands)} candidate site(s) (unverified)")
        for p in cands:
            print(f"  0x{base + p:X}")
        return

    funcs = load(BIN)
    hits = verify(cands, code, base, lo, hi, funcs)
    print(f"{len(hits)} reference(s) to [{lo:#x}, {hi:#x}]")
    for site, fn, t, text in hits:
        print(f"  {site:#x}  in function {fn:#x} (+{site - fn:#x})  -> {t:#x}   {text}")


if __name__ == "__main__":
    main()
