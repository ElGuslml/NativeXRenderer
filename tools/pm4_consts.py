"""PM4 type-3 headers and type-0 register writes built as constants (lis + ori/addi) per function.

  python xenos_native/tools/pm4_consts.py [LO HI]     (default: the whole image)

The first step for a new executable: every packet type the XDK D3D library emits is a lis/ori pair, and
the draw, resolve, clear, tiling and query functions each have a unique mix. Noisy: float constants decode
as headers too.

Prints, per function, the type-3 opcodes (with count) and type-0 register ranges found in its constants.
Needs out/ppc_index.pkl (python tools/ppc_index.py build).
"""
import collections, os, pickle, re, sys

funcs = pickle.load(open(os.environ.get("XN_INDEX", os.path.join("out", "ppc_index.pkl")), "rb"))

OPC = {0x10: "NOP", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMM",
       0x2D: "SET_CONSTANT", 0x55: "SET_CONSTANT2", 0x2F: "LOAD_ALU_CONSTANT", 0x56: "SET_SHADER_CONSTANTS",
       0x3B: "INVALIDATE_STATE", 0x3C: "WAIT_REG_MEM", 0x3D: "MEM_WRITE", 0x3F: "INDIRECT_BUFFER",
       0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x5A: "EVENT_WRITE_EXT", 0x5B: "EVENT_WRITE_ZPD",
       0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT", 0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI",
       0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI", 0x54: "INTERRUPT", 0x48: "ME_INIT",
       0x21: "REG_RMW", 0x45: "COND_WRITE", 0x4C: "CONTEXT_UPDATE", 0x4B: "SET_SHADER_BASES",
       0x3E: "COND_EXEC", 0x59: "?59", 0x44: "?44", 0x24: "VIZ_QUERY", 0x23: "?23", 0x2C: "?2C",
       0x37: "?37", 0x57: "?57", 0x47: "?47", 0x26: "?26", 0x25: "?25", 0x34: "DRAW_INDX_BIN",
       0x35: "DRAW_INDX_2_BIN", 0x4A: "?4A", 0x3A: "INVALIDATE?"}

LIS = re.compile(r"^lis r(\d+),(-?\d+)$")
LO = re.compile(r"^(ori|addi) r(\d+),r(\d+),(-?\d+)$")


def consts(ins):
    last_lis = {}
    out = []
    for addr, asm in ins:
        asm = asm.strip()
        m = LIS.match(asm)
        if m:
            last_lis[int(m.group(1))] = (int(m.group(2)) & 0xFFFF) << 16
            out.append((addr, last_lis[int(m.group(1))]))
            continue
        m = LO.match(asm)
        if m and int(m.group(3)) in last_lis:
            hi = last_lis[int(m.group(3))]
            lo = int(m.group(4))
            v = (hi | (lo & 0xFFFF)) if m.group(1) == "ori" else (hi + lo) & 0xFFFFFFFF
            out.append((addr, v))
    return out


def describe(v):
    t = v >> 30
    if t == 3:
        opc = (v >> 8) & 0x7F
        cnt = ((v >> 16) & 0x3FFF) + 1
        return f"T3 {OPC.get(opc, hex(opc))}[{cnt}]{' pred' if v & 1 else ''}"
    if t == 0 and (v >> 16) & 0x3FFF <= 0x3F and 0x2000 <= (v & 0x7FFF) < 0x2400 or (t == 0 and 0x4000 <= (v & 0x7FFF) < 0x4A00 and (v >> 16) < 0x200):
        return f"T0 reg {v & 0x7FFF:04X}+{((v >> 16) & 0x3FFF) + 1}"
    return None


def main():
    lo = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xFFFFFFFF
    for fn in sorted(funcs):
        if not lo <= fn < hi:
            continue
        found = collections.OrderedDict()
        for addr, v in consts(funcs[fn]):
            d = describe(v)
            if d and (v & 0xFFFF) != 0 or (d and d.startswith("T3")):
                found.setdefault(d, addr)
        if found:
            print(f"sub_{fn:08X}: " + ", ".join(f"{d}@{a:08X}" for d, a in found.items()))


if __name__ == "__main__":
    main()
