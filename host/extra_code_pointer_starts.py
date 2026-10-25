#!/usr/bin/env python3
"""Function entries the upstream lift misses: code pointers held as immediates.

The game's Lua bindings store small getters as plain addresses (`push imm32`,
`mov r/m32, imm32`) and call them indirectly, e.g. 0x0086fc60
(`mov eax, [0x00c71678]; ret`, the Game getter). Ghidra never makes those
functions, so the first mod that calls such an API traps with "indirect call
... resolves to neither a host shim nor a lifted function".

This scans .text linearly (resyncing on undecodable bytes) for immediate
operands that point back into .text at a 16-byte-aligned address preceded by
int3/ret/nop padding -- the shape of an MSVC function entry -- and prints the
ones missing from the lift's starts list, one per line, for appending to
output/recomp/lift/starts_union.txt before lift_parallel.py.

    python host/extra_code_pointer_starts.py \\
        --exe build/browsing-port/repo/tools/isaac-ng.unpacked.exe \\
        --starts build/browsing-port/repo/output/recomp/lift/starts_union.txt
"""
import argparse
import sys

import capstone
import pefile


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--starts", required=True)
    args = ap.parse_args()

    pe = pefile.PE(args.exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0").startswith(b".text"))
    tva = base + text.VirtualAddress
    tdata = text.get_data()
    tend = tva + text.Misc_VirtualSize
    starts = {int(line, 16) for line in open(args.starts) if line.strip()}

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    found = {}
    off = 0
    while off < len(tdata):
        decoded = False
        for insn in md.disasm(tdata[off:off + 4096], tva + off):
            decoded = True
            if insn.mnemonic in ("push", "mov"):
                for op in insn.operands:
                    if op.type != capstone.x86.X86_OP_IMM:
                        continue
                    v = op.imm & 0xFFFFFFFF
                    if not (tva < v < tend) or v in starts or v % 16:
                        continue
                    if tdata[v - tva - 1] in (0xCC, 0xC3, 0x90):
                        found.setdefault(v, insn.address)
            off = insn.address + insn.size - tva
        if not decoded:
            off += 1
    for v in sorted(found):
        print("0x%08x" % v)
    print("%d missing entries" % len(found), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
