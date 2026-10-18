# Edge network tile at a neighbor connection (SC4 1.1.641)

Static analysis of the apply routine `0x4aa6d0` (record DoMessage path, see connections-641.md §2) and what it calls. Ghidra copy in the scratchpad, disassembly read instruction by instruction because the decompiler loses the stack here. Nothing has been run in-game yet. Prior art: tloriato/OpenSC4 `docs/specs/network-tools.md` documents the same tool slots and agrees with everything below.

## Queue entry and the cells it touches

Entry `{type, x, z, add}` holds the cell **just outside** B's edge. `W = CellCountX()-1`, `H = CellCountZ()-1` (city slots 93/94).

| Edge (test order) | side | Line laid (start, then end) | Region demolished |
|---|---|---|---|
| `x < 0` west | 0 | `(0,z)` to `(2,z)` | x 0..2, row z |
| `z < 0` north | 1 | `(x,0)` to `(x,2)` | z 0..2, column x |
| `x > W` east | 2 | `(W,z)` to `(W-2,z)` | x W-2..W |
| else south | 3 | `(x,H)` to `(x,H-2)` | z H-2..H |

The start cell `(sx,sz)` is the edge cell inside the city. Masks: `clr = 0xFF << side*8`, `set = 2 << side*8`.

Type map (connection type → network type; layer is the demolition filter): 1→Rail 1, 2→Road 0, 3→Hwy 2, 4→Power 5, 5→Pipe 4 (layer 3), 6→Subway 7 (layer 2), 7→LRT 8, 8→Ave 6, 10→Dirt 11, 11→Mono 9, 12→GndHwy 12. Every other type (ferry) only calls `CreateConnection` (`0x4aad4b`). Layer is 1 by default.

## `dir` and the "primary" bit

Two bitsets are built per queue index. `primary` starts all 1 and `dir` starts all 0 (`0x478140(n, 1)` / `(n, 0)`). Only entries with add=1 and a dual type (3, 8, 12) change them. A counter runs over those entries: odd → primary=0, even → primary=1. Then `dir = primary`, inverted if the edge is north or east (`x >= 0 && z <= H`), and inverted again if `byte [0x00b21cc1] == 0` (drive side).

So, for **single networks, `dir` = 0 and primary = 1** (now confirmed). For a dual pair, only the second entry of the pair demolishes and lays the line. The first entry only gets `CreateConnection`.

## add=1 (`0x4aaa9a` to `0x4ab3d0`), exact order

| # | Call | Notes |
|---|---|---|
| 1 | `nm = city->GetNetworkManager()` (`0x602680` = city slot 38) | |
| 2 | `tool = nm->GetNetworkTool(netType, false)`; `tool->AddRef()` | false returns the shared tool at `nm+0x88+4*type`, **not** AddRef'd by the getter |
| 3 | `if (nsim->FindConnection(type,x,z)) skip` (slot 11) | skip = step 13 |
| 4 | if primary: `trafficMap = city->GetTrafficNetwork()` (slot 40, returns `city+0x1e8` = cSC4TrafficNetworkMap, clsid `0x6A0F82B2`) | |
| 5 | Build `SC4CellRegion<long>` over the 3 cells, all bits 1. Clear a bit when: power: `occMgr->GetFirstOccupantByStandardCityCell(o,cx,cz,0x2890D4DE)` (pole) or `(...,0x0990B2EA)` (power-line wire, built by PlacePoles `0x651560`) finds one. Other types: `trafficMap slot 11 (cx,cz)` (`0x4ae140`, = first dword of tile info) != 0, i.e. a network is already there | occMgr slot 33 (`+0x84`), the `uint32_t` overload |
| 6 | `filter = new layer filter` (vtables `0xa8fad4`/`0xa8fabc`, `+0xc` = layer); `IsOccupantIncluded(o)` = `layer(o) == L` (`0x5e9ca0`: 3 for pipe-class occupants, 2 for subway, else 1); `IsOccupantTypeIncluded` and `IsPropertyHolderIncluded` return true | game class, not public |
| 7 | `demolition->DemolishRegion(true, region, 0, 1, false, filter, nullptr, 0, 0, 0, 0)` (slot 6, `0x4aae96`) | free, no cost pointer |
| 8 | `tool->Init()` (slot 3) | Init is idempotent (flag `+0x18`) |
| 9 | power only: `zoneMgr->PlaceZone(region, 0 /*none*/, true, true, false, true, false, 0, 0, 0)` (slot 11) | dezones the line cells |
| 10 | `filter->Release()` | |
| 11 | `nsim->CreateConnection(type, x, z, dir)` (slot 6, `0x4aaf8b`) | done for every add=1 entry, primary or not |
| 12 | if primary: |  |
| 12a | `tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, -1)` (slot 7, `0x4aaff3`) | real placement, free, tool's own type |
| 12b | Verify: `occMgr->GetOccupantsByStandardCityCells(list, &s, &s, T, -1)` (slot 41) on the start cell, with `T = 0x2890D4DE` for power and `0xC772BF98` (network) otherwise. Power: any hit counts. Others: QI `0xA821EF94` and `IsOfType(netType)` (slot 22). With no hit: `nsim->DeleteConnection(type,x,z)` (slot 7) | the connection is rolled back if no piece landed |
| 12c | Same query with `T = 0xC772BF98`. For each network occupant with `IsOfType(netType)`: `f = GetEdgeTypesFlag(netType)` (slot 38), `SetEdgeTypesFlag(netType, (f & ~clr) \| set)` (slot 37). Remember `f & 0x04040404` | marks the side facing the neighbor |
| 12d | If any `0x04` bit: step one cell (byte0 x-1, byte1 z-1, byte2 x+1, byte3 z+1) and apply the same set there | partner cell of a multi-cell piece (inferred) |
| 12e | `tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, -1)` again | re-solves with the new edge flags |
| 13 | `tool->Shutdown()` (slot 4); `tool->Release()` | |

After the loop: the queue is cleared and `0x6AC284F3` is posted (`[0x00b43ccc]` slot 4). The list nodes come from the allocator at `[0x00b207e4]`; the vendored SC4List handles this itself.

## add=0 (`0x4ab41b` to `0x4ab664`)

This **does not remove occupants**, which corrects connections-641.md. The sequence:

1. `FindConnection(type,x,z)` must be non-null.
2. `DeleteConnection(type,x,z)` (slot 7).
3. Start cell, `0xC772BF98` occupants with `IsOfType(netType)`: `SetEdgeTypesFlag(netType, f & ~clr)`. The `0x04` partner cell is cleared the same way.
4. `tool->Init()`; `PlaceNetworkOccupants(sx, sz, sx, sz, 0, nullptr, true, -1)` (one cell, re-solve as a dead end); `Shutdown`; `Release`.

There is no primary test, so both cells of a dual pair do this.

## Globals and public replacements

| Global | What | Public route |
|---|---|---|
| `0x00b43cec` | current cISC4City (vtable `0x00a87620`, set by `0x601c10`) | `cISC4App::GetCity()` |
| `0x00b43d00` | city slot 31 GetDemolitionUtility | same getter |
| `0x00b43d0c` | city slot 34 GetOccupantManager | same getter |
| `0x00b43d14` | city slot 36 GetZoneManager | same getter |
| `0x00b43d8c` | city slot 62 GetNeighborsSimulator | same getter |
| `0x00b21cc1` | drive-side byte (dual `dir` only) | none needed for singles |
| `0x00b43ccc` | message server (post `0x6AC284F3`) | optional, skip |
| city slot 40 | cSC4TrafficNetworkMap (IID `0xCA0F81B9`) | header returns `intptr_t`, with no interface. Replace with occMgr `GetFirstOccupantByStandardCityCell(o, cx, cz, 0xC772BF98)` |

The slot cache at `0x601c20` confirms that the vendored cISC4City getters 31/34/36/38/40/62/93/94 are at the right offsets.

## Header check

| Interface | Slot (offset) | Header name | Binary (`0x00aa8748` base tool) | Verdict |
|---|---|---|---|---|
| NetworkTool | 3 (+0x0c) | Init | `0x6269c0` (pipe `0x657770`, power `0x650630`) | ok |
| NetworkTool | 4 (+0x10) | Shutdown | `0x626d30` (power `0x64f0d0`) | ok |
| NetworkTool | 5 (+0x14) | Reset | `0x626f90` | ok, not used here |
| NetworkTool | 7 (+0x1c) | PlaceNetworkOccupants | `0x629210`, pops 32, returns AL | slot ok. **The 7th arg `bUseWorldCache` is really `commit`** (true = build, false = draft) |
| NetworkTool | 18/19/22 | DrawNetworkLine / PlaceAllSegments / SetAnchorPoints | `0x63af40` / `0x63bdc0` / `0x62ca20` | ok, **not used** by `0x4aa6d0` |
| NetworkManager | 5 (+0x14) | GetNetworkTool(type, bCreateUnique) | `0x6108d0` | ok. Neither branch AddRefs: caller AddRefs and Releases |
| NeighborsSim | 6 / 7 / 11 | CreateConnection / DeleteConnection / FindConnection | as listed | ok |
| Demolition | 6 (+0x18) | DemolishRegion (11 args) | matches arg count and order | ok |
| ZoneManager | 11 (+0x2c) | PlaceZone (10 args) | matches | ok |
| OccupantManager | 33 (+0x84), 41 (+0xa4), 48 (+0xc0) | GetFirstOccupantByStandardCityCell / GetOccupantsByStandardCityCells / ReleaseOccupantList | used with a `uint32_t` type, and MSVC places overloads in reverse order, so the `uint32_t` overloads land here when compiled | ok |
| NetworkOccupant | 22 / 37 / 38 | IsOfType / SetEdgeTypesFlag / GetEdgeTypesFlag | `+0x58` / `+0x94` / `+0x98` | ok |
| OccupantFilter | 3-5 | IsOccupantIncluded / IsOccupantTypeIncluded / IsPropertyHolderIncluded | game class adds slot 6 = deleting destructor, which nothing calls through the interface | ok |

## Pipes and power

- **Pipe** (conn 5, net 4). The pipe tool (`0x00aa99a8`, underground base) places at its exemplar depth. The demolition filter is layer 3, so surface buildings and roads are not touched. Verification and edge flags use network occupants of type 4. The marker (in CreateConnection) gets props `0xCAA45670-72`.
- **Power** (conn 4, net 5). The power-line tool (`0x00aa9f30`) skips the base solve and runs `DeterminePolePositions` and `PlacePoles` (OpenSC4 network-tools.md §9). Pole-span rule: needs at least 2 steps and at least 48 units, which the 3-cell line meets in the game's own call. Demolition skips cells that already hold a pole or wire, then the cells are dezoned. The verification looks for a pole (`0x2890D4DE`) on the edge cell, not a network occupant. The edge-flag loop still queries `0xC772BF98`, so it probably does nothing for power.
- **Subway** (conn 6, net 7): layer 2, otherwise like a pipe.

## Feasibility: yes, with vendored interfaces only

The plugin can reproduce the add=1 sequence without calling `0x4aa6d0`. Three local pieces are needed: a small `cISC4OccupantFilter` (layer test), the cell check through the occupant manager instead of the traffic map, and `SC4CellRegion<long>` (vendored, 0x24 bytes, matching the game's rect + cRZCellMap). The order, for a single network on one queue entry:

```
city = app->GetCity(); nsim = city->GetNeighborsSimulator(); occ = city->GetOccupantManager();
if (nsim->FindConnection(type, x, z)) return;                     // already there
side/sx,sz,ex,ez/clr/set from the table above; netType from the map
tool = city->GetNetworkManager()->GetNetworkTool(netType, true);   // private: see risk 1
tool->AddRef();
SC4CellRegion<long> r(min(sx,ex), min(sz,ez), max(sx,ex), max(sz,ez), true);
  clear cells that already hold a network (occ->GetFirstOccupantByStandardCityCell(o,cx,cz,0xC772BF98)),
  or for power a pole/wire (0x2890D4DE / 0x0990B2EA)
city->GetDemolitionUtility()->DemolishRegion(true, r, 0, 1, false, &layerFilter, nullptr, 0, nullptr, 0, 0);
tool->Init();
if (power) city->GetZoneManager()->PlaceZone(r, 0, true, true, false, true, false, nullptr, nullptr, 0);
nsim->CreateConnection(type, x, z, false);                          // dir = 0 for singles
tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, (eNetworkType)-1);
verify edge cell (12b); if no piece: nsim->DeleteConnection(type, x, z) and stop
for each network occupant at (sx,sz) with IsOfType(netType):
    f = o->GetEdgeTypesFlag(netType); o->SetEdgeTypesFlag(netType, (f & ~clr) | set);  // + 0x04 partner cell
tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, (eNetworkType)-1);
tool->Shutdown(); tool->Release();
```

Delete (mirrors add=0): `DeleteConnection`, clear the side byte on the edge cell's pieces, then `Init`, a one-cell `PlaceNetworkOccupants(sx,sz,sx,sz,0,nullptr,true,-1)`, `Shutdown`, `Release`.

Risks and choices:

1. **Shared versus private tool.** The game uses `GetNetworkTool(t, false)`, then `Init` and `Shutdown`. Mid-game, `Shutdown` on the shared tool could break the player's active tool of that type. OpenSC4's harness uses `true` (private) and gets the same placement. Prefer `true`.
2. **Demolition.** A KISS first slice can skip `DemolishRegion` and the filter: if any of the 3 cells is not empty (`occ->IsCellEmpty`, or an occupant query), log it and skip the build. This drops the filter class entirely.
3. **Duals** (Ave, Hwy, GndHwy): two entries, CreateConnection on both. Lay the line only from the second, with `dir` from the formula above (needs `0x00b21cc1`, or read it from the source connection). Leave duals for later.
4. Run it from a cheat or the director's DoMessage on the main thread, inside try/catch. Log the `PlaceNetworkOccupants` return (AL) and `GetFailureState()` (slot 14).
5. A code-placed piece is free and usable on return (OpenSC4 §3.4, §6.3).

## Prior art (Q6)

- **NAMTeam/nam-dll** and **0xC0000054** (Null45): GitHub code search finds no use of `PlaceNetworkOccupants`, `DrawNetworkLine`, `PlaceAllSegments` or `GetNetworkTool`. `nam-dll/src/NetworkStubs.h` has none of them either.
- **tloriato/OpenSC4** `engine/plugin/FC4Script.cpp` uses this pattern (`network` script op):
  ```
  tool = netMgr->GetNetworkTool(type, true); AddRef
  tool->Init();                                                       // slot 3
  ok = tool->PlaceNetworkOccupants(x0, z0, x1, z1, 0, 0, 1, 0xFFFFFFFF) & 0xFF;  // slot 7
  tool->Shutdown();                                                   // slot 4
  Release
  ```
  Its spec notes: "makes no regional (neighbour) connection", "free", "inserts the occupants before returning".
- **kycube/SiMCP-4** `re/reports/commands/REPORT.md`: the game's own PlaceNetwork command does `GetNetworkTool(type,false)`, `Init`, then `PlaceNetworkOccupants(x1,z1,x2,z2,0,0,true,-1)`.
