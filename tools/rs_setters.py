"""Render-state setters of an XDK D3D library, named by the shadow registers they write.

  python native_x_renderer/tools/rs_setters.py LO HI GROUPS [register_table.inc]

GROUPS is the device's register shadow, "offset:first_register:count" comma-separated (decimal, hex,
decimal), as in the game's native_x_renderer::DeviceLayout. For XDK 2.0.5426 (POTCAWE):
  10240:2000:16,10316:2100:21,10400:2180:5,10420:2200:12,10468:2280:21,10552:2300:38,10704:2380:8
The register table defaults to thirdparty/rexglue-sdk/include/rex/graphics/register_table.inc.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fn_summary  # noqa: E402

REGS = {}
GROUPS = []


def reg_at(off):
    for base, reg, n in GROUPS:
        if base <= off < base + 4 * n:
            r = reg + (off - base) // 4
            return f"{REGS.get(r, '?')}({r:04X})"
    return None


def main():
    if len(sys.argv) < 4:
        return print(__doc__)
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    for g in sys.argv[3].split(","):
        off, reg, n = g.split(":")
        GROUPS.append((int(off), int(reg, 16), int(n)))
    table = sys.argv[4] if len(sys.argv) > 4 else "thirdparty/rexglue-sdk/include/rex/graphics/register_table.inc"
    for line in open(table):
        m = re.match(r"XE_GPU_REGISTER\((0x[0-9A-F]+), \w+, (\w+)\)", line)
        if m:
            REGS[int(m.group(1), 16)] = m.group(2)
    for fn in sorted(f for f in fn_summary.funcs if lo <= f < hi):
        s = fn_summary.summarize(fn)
        w = re.search(r"\n    W (.*)\n", s).group(1).split()
        regs = [reg_at(int(x.split("+")[1])) for x in w]
        regs = [r for r in dict.fromkeys(regs) if r]
        if regs:
            print(s.split("\n")[0][:90], "->", " ".join(regs))


if __name__ == "__main__":
    main()
