"""Index a ReXGlue game's generated C++ into functions -> (address, PPC instruction) and query it.

The codegen keeps every PPC instruction as a "// asm" comment, one per instruction. Blocks are not always
emitted in address order, so the "loc_XXXXXXXX:" labels anchor the addresses; "bl" lines are cross-checked
against the "ctx.lr = 0x..." the codegen writes for the return address.

Run from the game's repository (paths are relative to the current directory):

  python xenos_native/tools/ppc_index.py build [generated/default]  # -> out/ppc_index.pkl
  python xenos_native/tools/ppc_index.py range LO HI     # functions in a range, with size and callers
  python xenos_native/tools/ppc_index.py dis ADDR...     # functions, with addresses
  python xenos_native/tools/ppc_index.py callers ADDR... # call sites (bl / b) of functions
  python xenos_native/tools/ppc_index.py grep REGEX [LO HI]  # instructions matching REGEX

Addresses are hexadecimal without 0x (822B58B0). The index path can be changed with XN_INDEX.
"""
import collections, os, pickle, re, sys

GEN = os.path.join("generated", "default")
PKL = os.environ.get("XN_INDEX", os.path.join("out", "ppc_index.pkl"))

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
ASM_RE = re.compile(r"^\t// (.*)$")
LR_RE = re.compile(r"ctx\.lr = 0x([0-9A-F]{8});")
LABEL_RE = re.compile(r"^loc_([0-9A-F]{8}):")


def build(gen=GEN):
    funcs = {}
    bad = 0
    for name in sorted(os.listdir(gen)):
        if not re.match(r".+_recomp\.\d+\.cpp$", name):
            continue
        cur = None
        pending_bl = None
        with open(os.path.join(gen, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = int(m.group(1), 16)
                    next_addr = cur
                    funcs[cur] = []
                    pending_bl = None
                    continue
                if cur is None:
                    continue
                m = LABEL_RE.match(line)
                if m:
                    # Blocks are not always emitted in address order: a label anchors the next one.
                    next_addr = int(m.group(1), 16)
                    continue
                m = ASM_RE.match(line)
                if m:
                    addr = next_addr
                    next_addr += 4
                    funcs[cur].append((addr, m.group(1)))
                    pending_bl = addr if m.group(1).startswith("bl ") else None
                    continue
                if pending_bl is not None:
                    m = LR_RE.search(line)
                    if m:
                        if int(m.group(1), 16) != pending_bl + 4:
                            bad += 1
                        pending_bl = None
    os.makedirs(os.path.dirname(PKL), exist_ok=True)
    with open(PKL, "wb") as f:
        pickle.dump(funcs, f)
    for v in funcs.values():
        v.sort()
    n = sum(len(v) for v in funcs.values())
    print(f"{len(funcs)} functions, {n} instructions, {bad} bl/lr mismatches -> {PKL}")


def load():
    with open(PKL, "rb") as f:
        return pickle.load(f)


BRANCH_RE = re.compile(r"^(bl|b)\s+0x([0-9a-fA-F]{8})\s*$")


def call_sites(funcs):
    """target -> [(site address, caller function, 'bl' or 'b')]"""
    sites = collections.defaultdict(list)
    for fn, ins in funcs.items():
        for addr, asm in ins:
            m = BRANCH_RE.match(asm)
            if m:
                t = int(m.group(2), 16)
                if t in funcs and t != fn:
                    sites[t].append((addr, fn, m.group(1)))
    return sites


def h(x):
    return f"{x:08X}"


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    if cmd == "build":
        return build(sys.argv[2] if len(sys.argv) > 2 else GEN)
    if cmd == "help":
        return print(__doc__)
    funcs = load()
    if cmd == "dis":
        for a in sys.argv[2:]:
            fn = int(a, 16)
            print(f"--- sub_{h(fn)} ({len(funcs[fn])} instructions)")
            for addr, asm in funcs[fn]:
                print(f"  {h(addr)}  {asm}")
    elif cmd == "callers":
        sites = call_sites(funcs)
        for a in sys.argv[2:]:
            fn = int(a, 16)
            print(f"--- sub_{h(fn)}: {len(sites[fn])} call sites")
            for addr, caller, kind in sorted(sites[fn]):
                print(f"  {h(addr)} in sub_{h(caller)} ({kind})")
    elif cmd == "range":
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        sites = call_sites(funcs)
        for fn in sorted(f for f in funcs if lo <= f < hi):
            inside = sum(1 for _, c, _ in sites[fn] if lo <= c < hi)
            outside = len(sites[fn]) - inside
            print(f"sub_{h(fn)} {len(funcs[fn]):5d} ins  callers in-range {inside:4d}  outside {outside:4d}")
    elif cmd == "grep":
        rx = re.compile(sys.argv[2])
        lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
        hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xFFFFFFFF
        for fn in sorted(funcs):
            if not lo <= fn < hi:
                continue
            for addr, asm in funcs[fn]:
                if rx.search(asm):
                    print(f"sub_{h(fn)} {h(addr)}  {asm}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
