"""Reads the XenosNative trace dumps of a log (--xenos_native_trace, F9) and prints a frame's structure.

  python xenos_native/tools/xn_trace.py LOG                  # list the dumped frames
  python xenos_native/tools/xn_trace.py LOG FRAME passes     # render target passes: binds, clears, resolves, draw counts
  python xenos_native/tools/xn_trace.py LOG FRAME counts     # calls by kind (outermost / nested)
  python xenos_native/tools/xn_trace.py LOG FRAME calls [REGEX]   # the raw lines (optionally filtered)
"""
import collections, re, sys

LINE = re.compile(r"\[xenos_native\] #(\d+) ( *)(\w+) ?(.*)$")
START = re.compile(r"trace: dumping frames (\d+)\.\.(\d+)")
END = re.compile(r"trace: end of dumped frame (\d+) \((\d+) calls\)")


def frames(path):
    out, cur = collections.OrderedDict(), []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if m:
            cur.append(dict(seq=int(m.group(1)), depth=len(m.group(2)) // 2, name=m.group(3), rest=m.group(4)))
            continue
        m = END.search(line)
        if m:
            out[int(m.group(1))] = cur
            cur = []
    return out


def arg(rest, key):
    m = re.search(r"(?:^| )" + key + r"=(\S+)", rest)
    return m.group(1) if m else None


def passes(calls):
    # Calls come in completion order (a nested call prints before its parent); sort by sequence of
    # completion is what we have, which is fine for outermost calls.
    rt = ds = None
    draws = collections.Counter()
    def flush():
        if sum(draws.values()):
            print(f"      draws: {dict(draws)}")
            draws.clear()
    for c in calls:
        n, r, d = c["name"], c["rest"], c["depth"]
        if n == "SetRenderTarget" and arg(r, "index") == "0x0":
            flush(); rt = arg(r, "surface")
            info = re.search(r"\[surface info=(\S+) color/depth info=(\S+)\]", r)
            print(f"  RT0 = {rt} {('info=' + info.group(1) + ' color=' + info.group(2)) if info else ''}")
        elif n == "SetDepthStencilSurface":
            flush(); ds = arg(r, "surface")
            info = re.search(r"\[surface info=(\S+) color/depth info=(\S+)\]", r)
            print(f"  DS  = {ds} {('info=' + info.group(1) + ' depth=' + info.group(2)) if info else ''}")
        elif n in ("Clear", "ClearF", "Resolve", "BeginTiling", "EndTiling", "BeginZPass", "EndZPass", "SetPredication", "QueryIssue"):
            flush()
            print(f"  {'  ' * d}{n} {r.split(' | ')[0][:200]}")
        elif n.startswith("Draw") or n == "BeginVertices":
            if d == 0:
                draws[n] += 1
    flush()


def main():
    path = sys.argv[1]
    fr = frames(path)
    if len(sys.argv) == 2:
        for f, calls in fr.items():
            c = collections.Counter(x["name"] for x in calls if x["depth"] == 0)
            print(f"frame {f}: {len(calls)} calls, draws {c['DrawIndexedVertices']}+{c['DrawVertices']}+{c['DrawVerticesUP']}")
        return
    calls = fr[int(sys.argv[2])]
    mode = sys.argv[3]
    if mode == "counts":
        c = collections.Counter((x["name"], x["depth"] > 0) for x in calls)
        for (n, nested), k in sorted(c.items()):
            print(f"{n:28s} {'nested' if nested else 'outer ':6s} {k}")
    elif mode == "passes":
        passes(calls)
    elif mode == "calls":
        rx = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
        for x in calls:
            s = f"#{x['seq']} {'  ' * x['depth']}{x['name']} {x['rest']}"
            if not rx or rx.search(s):
                print(s[:400])


if __name__ == "__main__":
    main()
