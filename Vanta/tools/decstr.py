#!/usr/bin/env python3
"""Decrypt RobloxPlayer's inline stack-encrypted string literals.

    tools/decstr.py <fn_addr> <byte_cap>

Some string constants aren't in __cstring at all — the compiler emits them as
a run of `mov byte ptr [rbp-N], imm8` stores building the ciphertext on the
stack, followed by a small per-byte transform loop (index-derived rotate,
then xor by a constant) decrypting it into a heap buffer just before it's
turned into a std::string. This finds those store runs in a disassembled
range, recovers the transform from the instructions that follow each run,
and replays it in Python.

Read the range with the offset dumper's own disassembler first (see the
snippet in detect.md's Method notes / this file's own use in that
investigation) so `insn.text` is already normalized the way this expects.
"""
import re, sys, os

sys.path.insert(0, "/Users/evilion/Documents/unix-roblox/offset-dumper")
from nexadump import context

BIN = os.environ.get("ROBLOX_BIN", "/Applications/Roblox.app/Contents/MacOS/RobloxPlayer")

_imm = lambda s: int(s.strip(), 0)

_ST_IMM = re.compile(r"^mov byte ptr \[rbp - (0x[0-9a-f]+)\], (0x[0-9a-f]+|\d+)$")
_ST_REG = re.compile(r"^mov byte ptr \[rbp - (0x[0-9a-f]+)\], ([a-d]l|[a-d]h|sil|dil|r\d+b)$")
_LD_REG = re.compile(r"^mov ([a-d]l|sil|dil|r\d+b), (0x[0-9a-f]+|\d+)$")
_CL_OP = re.compile(r"^(and|xor|or|add|sub|shl|shr) cl,")
_DL_OP = re.compile(r"^(ror|rol|xor|add|sub) dl,")


def _rot(v, n, right):
    n &= 7
    return ((v >> n) | (v << (8 - n))) & 0xff if right else ((v << n) | (v >> (8 - n))) & 0xff


def find_blobs(ins):
    """Group consecutive stack-byte stores (imm or a just-loaded register)."""
    blobs, cur, start, regs = [], {}, None, {}
    for i, insn in enumerate(ins):
        t = insn.text
        m = _ST_IMM.match(t)
        if m:
            if not cur:
                start = i
            cur[-_imm(m.group(1))] = _imm(m.group(2)) & 0xff
            continue
        m = _ST_REG.match(t)
        if m and m.group(2) in regs:
            if not cur:
                start = i
            cur[-_imm(m.group(1))] = regs[m.group(2)]
            continue
        m = _LD_REG.match(t)
        if m:
            regs[m.group(1)] = _imm(m.group(2)) & 0xff
            continue
        if cur and not t.startswith(("nop", "mov al,", "mov cl,", "mov dl,", "mov bl,")):
            blobs.append((start, i, cur))
            cur = {}
    if cur:
        blobs.append((start, len(ins), cur))
    return blobs


def decrypt(ins, bs, be, d):
    """Recover and replay the transform loop that follows a store run."""
    data = [d[k] for k in sorted(d)]
    ops, n, bias = [], None, 0
    for insn in ins[be:be + 45]:
        t = insn.text
        if t.startswith("mov esi,") and n is None:
            try:
                n = _imm(t.split(",")[1])
            except ValueError:
                pass
        m = re.match(r"^lea (\w+), \[rax \+ (\d+)\]$", t)
        if m:
            bias = int(m.group(2))
        if _CL_OP.match(t) or t in ("inc cl", "dec cl", "not cl", "neg cl") \
                or _DL_OP.match(t) or t in ("not dl", "neg dl"):
            ops.append(t)
        if t.startswith("jne "):
            break

    out = bytearray()
    for i in range(len(data)):
        dl, cl = data[i], (i + bias) & 0xff
        for t in ops:
            op = t.split(",")[0].split()[0]
            tgt = t.split()[1].rstrip(",")
            v = None
            if "," in t:
                a = t.split(",", 1)[1].strip()
                if a != "cl":
                    try:
                        v = _imm(a)
                    except ValueError:
                        v = None
            if tgt == "cl":
                if op == "and": cl &= v
                elif op == "xor": cl ^= v
                elif op == "or": cl |= v
                elif op == "add": cl = (cl + v) & 0xff
                elif op == "sub": cl = (cl - v) & 0xff
                elif op == "shl": cl = (cl << v) & 0xff
                elif op == "shr": cl >>= v
                elif op == "inc": cl = (cl + 1) & 0xff
                elif op == "dec": cl = (cl - 1) & 0xff
                elif op == "not": cl = ~cl & 0xff
                elif op == "neg": cl = -cl & 0xff
            else:
                if op == "ror": dl = _rot(dl, cl if v is None else v, True)
                elif op == "rol": dl = _rot(dl, cl if v is None else v, False)
                elif op == "xor": dl ^= v
                elif op == "add": dl = (dl + v) & 0xff
                elif op == "sub": dl = (dl - v) & 0xff
                elif op == "not": dl = ~dl & 0xff
                elif op == "neg": dl = -dl & 0xff
        out.append(dl)
    return bytes(out), n


def main():
    fn, cap = int(sys.argv[1], 0), int(sys.argv[2], 0)
    b = context.Binary(BIN)
    ins = list(b.body(fn, cap))
    for bs, be, d in find_blobs(ins):
        if len(d) < 4:
            continue
        out, n = decrypt(ins, bs, be, d)
        txt = out.rstrip(b"\x00")
        ok = all(32 <= c < 127 for c in txt)
        print("%x  len=%-4s %-5s %r" % (ins[bs].addr, n if n is not None else len(d),
                                         "" if ok else "??", out))


if __name__ == "__main__":
    main()
