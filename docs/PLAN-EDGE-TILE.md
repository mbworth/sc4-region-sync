# Plan: lay the edge network piece for mirrored connections (slice 2b)

Research: `docs/research/edge-tile-641.md`. Today a mirrored connection is an arrow marker only; the player must lay the piece under it (which works, Q9). The game's load path lays a 3-cell stub from the edge cell inward with `cISC4NetworkTool::PlaceNetworkOccupants` and marks the edge-facing side of the piece with `SetEdgeTypesFlag`. This slice reproduces that for single networks. Reviewed 2026-10-18; the blockers (geometry, occupancy, pre-emption by the timed path) are folded in below.

## Scope

- Connection types: rail 1, road 2, power 4, pipe 5, subway 6, light rail 7, dirt road 10, monorail 11. Duals (avenue 8, highway 3, ground highway 12) and ferry stay arrow-only.
- Decision to lay is idempotent and independent of who created the connection: a connection of the type exists to that neighbor at that cell (`FindConnection`), **and** no network occupant of that network type sits on the edge cell. So it also covers arrow-only connections left in saves by the Q9 runs, and skips stubs the game laid itself at load.
- Runs in `MirrorConnections` after the connection step, for every connection in the snapshot that is ours to mirror (not only newly created ones), on the timed path behind `LayEdgeNetwork` (INI, default 0 until observed) and always from `RegionSyncMirror`, which bypasses the `mirroredConnections` identity set for this step.
- Removal: none.

## Connection type to network type (game-free, next to `ToSubscriberFrame`, with a `make test` case)

| Connection | Network (`eNetworkType`) |
|---|---|
| 1 rail | 1 Rail |
| 2 road | 0 Road |
| 4 power | 5 PowerPole |
| 5 pipe | 4 WaterPipe |
| 6 subway | 7 Subway |
| 7 light rail | 8 LightRail |
| 10 dirt road | 11 DirtRoad |
| 11 monorail | 9 Monorail |

## Geometry (game-free, tested)

`bx,bz` from `ToSubscriberFrame` is the cell just **outside** the edge (-1 or N). With W = N-1:

| Side | Condition | Edge cell `(sx,sz)` | Inner end `(ex,ez)` | Flag byte |
|---|---|---|---|---|
| west | `bx == -1` | `(0, bz)` | `(2, bz)` | 0 |
| north | `bz == -1` | `(bx, 0)` | `(bx, 2)` | 1 |
| east | `bx == N` | `(W, bz)` | `(W-2, bz)` | 2 |
| south | `bz == N` | `(bx, W)` | `(bx, W-2)` | 3 |

Cross-check N against `pCity->CellCountX()/CellCountZ()` and log a mismatch (the game uses the cell count, not the size enum).

## Sequence per connection

1. Skip unless `FindConnection(type, bx, bz)` is non-null.
2. Already laid: a network piece of the same type on the edge cell (power: a pole) means the stub is there; log `already laid`, skip. Otherwise survey the 3 cells and log every occupant (`GetOccupantsByStandardCityCells`, type 0 = all) and the zone type (`zoneMgr->GetZoneType`). Build `SC4CellRegion<int32_t>` over the 3 cells, all bits set, and clear the bit of a cell that already holds a network piece of any type (`GetFirstOccupantByStandardCityCell`, `0xC772BF98`; power: pole `0x2890D4DE` or wire `0x0990B2EA`), as research step 5 does. (Amended 2026-10-18: demolish like the game instead of skipping occupied cells. `IsCellEmpty` was dropped: it takes 64 m world cells, not standard cells.)
3. `tool = nm->GetNetworkTool(netType, true)`, `AddRef`; hold it in an RAII guard that calls `Shutdown` and `Release` on every exit. Null: log, skip. Log funds, then `demolition->DemolishRegion(true, region, 0, 1, false, &layerFilter, nullptr, 0, nullptr, 0, 0)` (research steps 6 and 7). The local filter (on `cSC4BaseOccupantFilter`) includes: for pipe, only pipe network occupants; for subway, only subway ones; otherwise any occupant that is not a pipe or subway piece. The call returns no list, so the occupants left on each cell are logged after it.
4. Log funds (`pCity->GetBudgetSimulator()`), then `tool->Init()`. Power only: `zoneMgr->PlaceZone(region, None, true, true, false, true, false, nullptr, nullptr, 0)` (research step 9). Other types: zone is logged only.
5. `tool->PlaceNetworkOccupants(sx, sz, ex, ez, 0, nullptr, true, (eNetworkType)-1)`; log the return, `GetFailureState()`, `GetFailureReasonString(cRZBaseString&)`.
6. Verify all 3 cells: `GetOccupantsByStandardCityCells(list, &cell, &cell, T, -1)` with `T = 0xC772BF98` (power: `0x2890D4DE`, any pole counts, no QI). For network types: QI `cISC4NetworkOccupant`, `IsOfType(netType)`. `ReleaseOccupantList` after each query, `Release` each QI'd pointer. Log per cell. If the edge cell has no piece: log `EdgeTile: no piece on the edge cell` and stop (the connection stays; that is today's state).
7. Edge flags (network types only, not power): for each matching occupant on the edge cell, `f = GetEdgeTypesFlag(netType)`, log `f`, `SetEdgeTypesFlag(netType, (f & ~(0xFFu << side*8)) | (2u << side*8))`, log `f & 0x04040404` for the record. The partner-cell step (research 12d) is deferred until a log shows the bit.
8. `PlaceNetworkOccupants` again, same arguments; log the return. Log funds again.
9. Guard releases the tool.

All inside the existing mirror try/catch; every game call logged at Info before and after (first exercise of these methods). Pass occupant type ids as `uint32_t` explicitly (overload with the filter pointer).

## What must not break

- The player's active network tool and any drag preview: private tool instance only; never `Shutdown` the shared one.
- Player work: only the 3 edge cells are touched, with the game's own layer filter, and cells already holding a network are left out. The game demolishes the same cells (and dezones them for power) when it lays the stub at load, so this is vanilla behaviour, only earlier.
- Budget: placement and demolition by code are free (research, OpenSC4; no cost pointer); the funds log before and after confirms it.
- Half-built state: all 3 cells verified and logged; a partial lay is reported, not hidden.
- `NeighborSnapshot` stays game-free: the type map and geometry live there (pure functions), nothing else.

## Code placement

- `src/EdgeNetwork.{h,cpp}` (new): `LayEdgePiece(ctx, type, bx, bz, cells) -> {laid, reason}`; `MirrorContext` gains `pCity`.
- `MirrorConnections` calls it; `ConnectionMirrorResult` gains `laid`.
- `SyncSettings`: `LayEdgeNetwork`.

## Experiment (Q10 in OPEN-QUESTIONS)

Run with `SyncConnections=1` as shipped; the lay decision is idempotent, so the cheat lays under arrows the timed path already created.

1. Both instances, A builds a road to a bare stretch of the shared edge; B `RegionSyncMirror`. Expect `PlaceNetworkOccupants=1` twice, all 3 cells verified, a 3-cell road stub under the arrow, funds unchanged, and B can still drag a road afterwards. Then power (expect pole on the edge cell, no flag step), pipe and rail on bare cells.
2. A house on B's edge cell: expect it demolished (`DemolishRegion=1`, no occupants left on that cell), then the stub laid. A road crossing the stub cells: that cell kept (`kept`), the stub joins it. Power over a zoned cell: `PlaceZone=1`, cell dezoned.
3. Save B (A first, then B), reload both: stub persists, no second stub, the lay step logs `already laid`.
4. Flip `LayEdgeNetwork=1`, repeat 1 with no cheats.

## Deferred

- Duals and ferry; removal (add=0 sequence); the 0x04 partner cell.
