# NativeXRenderer tools

Offline analysis scripts for bringing up a game. They are Python 3 with no dependencies, contain no game
addresses, and run from the game's repository root (the paths are relative to it). They are the method of
[`../docs/integrating-a-game.md`](../docs/integrating-a-game.md), step 1 and step 3.

| Script | Does |
|---|---|
| `ppc_index.py` | Indexes the generated C++ (`generated/default/*_recomp.N.cpp`) into functions and instructions with addresses, in `out/ppc_index.pkl`. Then disassembles, lists call sites, lists the functions of a range with their caller counts, or greps instructions. Run `build` again after every codegen. |
| `pm4_consts.py` | Finds the PM4 packet headers and register writes each function builds as `lis`+`ori` constants. The first scan for a new executable: it finds the draws, the resolve, the clear, the tiling functions and the queries. |
| `fn_summary.py` | One summary per function: the device offsets it reads and writes, its calls, its PM4 constants, its callers. A setter is a function that writes one known field. |
| `rs_setters.py` | Names the render-state setters by the register they write in the device's register shadow (the shadow groups are an argument, as in the game's `DeviceLayout`). |
| `frame_trace.py` | Reads the trace's frame dumps from a log (`--native_x_renderer_trace`, F9): the list of dumps, a frame's passes (targets, clears, resolves, tiling, queries, draw counts), call counts, raw lines. |

```powershell
python native_x_renderer/tools/ppc_index.py build
python native_x_renderer/tools/pm4_consts.py 82000000 82600000 > out/pm4.txt
python native_x_renderer/tools/ppc_index.py dis 824AC3C0
python native_x_renderer/tools/fn_summary.py range 822AD000 822C7000 > out/d3d_summary.txt
python native_x_renderer/tools/rs_setters.py 822AD000 822C7000 10240:2000:16,10316:2100:21,10400:2180:5,10420:2200:12,10468:2280:21,10552:2300:38,10704:2380:8
python native_x_renderer/tools/frame_trace.py out/build/win-amd64-relwithdebinfo/logs/potcawe_013.log 2445 passes
```

The examples use POTCAWE's addresses (XDK 2.0.5426). `PPC_INDEX` changes where the index is kept.
