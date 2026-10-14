"""One-line summaries of functions: device offsets read/written, calls, PM4 constants, callers.

  python xenos_native/tools/fn_summary.py ADDR [...]       # explicit functions
  python xenos_native/tools/fn_summary.py range LO HI

"Device" is r3 at entry and any register copied from it (mr rN,r3) before r3 is overwritten; the
offsets are approximate (no real data flow), which is enough to classify setters.
Needs out/ppc_index.pkl (python tools/ppc_index.py build).
"""
import os, pickle, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm4_consts  # noqa: E402
from ppc_index import call_sites  # noqa: E402

funcs = pickle.load(open(os.environ.get("XN_INDEX", os.path.join("out", "ppc_index.pkl")), "rb"))
SITES = call_sites(funcs)

# The compiler's register save/restore helpers (__savegprlr_N / __restgprlr_N): called from every
# prologue right after "mflr r12", and they keep r3..r12. The restore helpers sit next to them.
def _save_restore():
    out = set()
    for ins in funcs.values():
        if len(ins) > 1 and ins[0][1].strip() == "mflr r12":
            m = re.match(r"^bl\s+0x([0-9a-fA-F]{8})", ins[1][1].strip())
            if m:
                out.add(int(m.group(1), 16))
    if out:
        lo, hi = min(out), max(out)
        out |= {f for f in funcs if lo <= f < hi + 0x100}
    return out


SAVE_RESTORE = _save_restore()

MEM = re.compile(r"^(lwz|lbz|lhz|ld|lwzu|stw|stb|sth|std|stwu|lfs|stfs|lfd|stfd|lha)\s+r?f?(\d+),(-?\d+)\(r(\d+)\)$")
MR = re.compile(r"^mr r(\d+),r(\d+)$")
WRITES_RD = re.compile(r"^(?!st|cmp|b|tw|mt)\w+\.?\s+r(\d+),")
CALL = re.compile(r"^(bl|b)\s+0x([0-9a-fA-F]{8})\s*$")


def summarize(fn):
    ins = funcs[fn]
    dev = {3}
    loads, stores = collections.OrderedDict(), collections.OrderedDict()
    calls = []
    for addr, asm in ins:
        asm = asm.strip()
        m = MR.match(asm)
        if m:
            d, s = int(m.group(1)), int(m.group(2))
            if s in dev:
                dev.add(d)
            elif d in dev:
                dev.discard(d)
            continue
        m = MEM.match(asm)
        if m:
            op, rd, off, base = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
            if base in dev and off >= 0:
                (stores if op.startswith("st") else loads)[f"{op[:-1] if op.endswith('u') else op}+{off}"] = 1
            if not op.startswith("st") and not op.startswith("lf") and rd in dev:
                dev.discard(rd)
            continue
        m = CALL.match(asm)
        if m:
            t = int(m.group(2), 16)
            if t in funcs and t != fn and t not in SAVE_RESTORE:
                calls.append(f"{m.group(1)} {t:08X}")
            if m.group(1) == "bl" and t not in SAVE_RESTORE:
                dev -= {3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
            continue
        m = WRITES_RD.match(asm)
        if m and int(m.group(1)) in dev:
            dev.discard(int(m.group(1)))
    pm4 = []
    for _, v in pm4_consts.consts(ins):
        d = pm4_consts.describe(v)
        if d and ((v & 0xFFFF) or d.startswith("T0")) and d not in pm4:
            pm4.append(d)
    callers = SITES[fn]
    outs = sorted({f"{c:08X}" for _, c, _ in callers})
    return (f"sub_{fn:08X} [{len(ins)} ins, {len(callers)} sites: {' '.join(outs[:6])}{' …' if len(outs) > 6 else ''}]\n"
            f"    R {' '.join(loads)}\n    W {' '.join(stores)}\n"
            f"    calls {' '.join(dict.fromkeys(calls))}\n    pm4 {', '.join(pm4)}")


def main():
    if sys.argv[1] == "range":
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        targets = sorted(f for f in funcs if lo <= f < hi)
    else:
        targets = [int(a, 16) for a in sys.argv[1:]]
    for fn in targets:
        print(summarize(fn))


if __name__ == "__main__":
    main()
