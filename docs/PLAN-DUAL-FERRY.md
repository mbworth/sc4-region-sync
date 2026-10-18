# Plan: dual-cell networks (avenue, highway, ground highway) and ferry

Research: `docs/research/dual-ferry-641.md`. Builds on `docs/PLAN-EDGE-TILE.md` and `docs/PLAN-REMOVAL.md`. Reviewed 2026-10-18 (mechanics and integration lenses); the first draft's delete-on-failure, fallback placement and in-loop lay were dropped on their findings.

## Facts the plan rests on

- Avenue (type 8, net 6), elevated highway (3, net 2) and ground highway (12, net 12) make **one connection object per lane**: two objects on adjacent cells just outside the same edge, each with its own marker. Nothing else distinguishes them from singles: same `CreateConnection`, same record entries, same `DeleteConnection`.
- `dir` is cosmetic (rotates the marker 180 degrees) and is not stored. The load path computes it from pair parity, edge and the Drive on Right byte at `0x00b21cc1` for lanes it creates; a lane that already exists keeps its marker, so a wrong dir persists in the save.
- On load the game lays the stub from the higher-coordinate lane (hi) only, with the dual net type over hi's 3-cell line; the width-2 tool widens toward the lower coordinate (lo), so hi's line covers lo's row (inferred from the drag code; vanilla's own load makes exactly this call). It demolishes hi's row only, sets the edge flag on hi's edge cell and on its 0x04 partner (lo's edge cell), and verifies hi's edge cell only.
- Garbage deals look for a connection of type 3, then 12, then 8, then 2, then 10: mirroring duals is what enables garbage deals over avenues and highways.
- Ferry (type 9) is created by each city from its own terrain (edge water runs), never by the player, has no marker and no network tile, deals never use it, and the load path creates a mirrored ferry even from a removal entry.

## Rules

### Game-free part (`NeighborSnapshot.h`, tested in `tests/snapshot_roundtrip.cpp`)

1. `IsMirrorableConnectionType` gains 3, 8, 12; `ConnectionToNetworkType` maps 3→2, 8→6, 12→12; `IsDualConnectionType(t)`. 9 stays unmirrorable and -1.
2. `PairDualLanes(lanes)`: input is the publisher's complete connection list in **the publisher's own frame** (so a lane whose partner lies on a third tile pairs with that partner, not with the next avenue over; the `otherTile` filter is applied to the result, not the input). Group by (type, publisher edge side), sort by the along-edge inner coordinate, dedupe, pair greedily from the lowest when coordinates differ by exactly 1. Returns (lo, hi) index pairs. Lanes whose partner did not pair, or whose pair does not convert as a whole to our edge, are unpaired.
3. `DualLaneDir(side, hi, driveOnRight) = hi ^ (side is north or east) ^ !driveOnRight` for our side (0 W, 1 N, 2 E, 3 S). Tests: all four sides × lo/hi × drive side, against the research table.
4. Tests to add: west pair listed hi first; pairs on all four edges; four adjacent lanes → two pairs; three adjacent → one pair plus one unpaired; a gap → none; type 8 next to type 3 → not paired; duplicate entry deduped; corner cell excluded; singles ignored; the straddler case (lanes at 63 and 64 of one edge pair with each other).

### Mirror (`NeighborMirror.cpp`), three passes per publisher

5. **Pair pass**: `PairDualLanes` over `a.connections`, then keep pairs whose both lanes convert to our edge (`ToSubscriberFrame`), giving per lane a role (lo, hi, unpaired, single) and a dir. `DriveOnRight` is read once per pass through a `src/*641.h` accessor of the byte at `0x00b21cc1` and logged on each pair line only.
6. **Create pass**: the existing per-entry loop, unchanged for singles, except that `CreateConnection` passes the lane's dir instead of `false`. Each lane keeps its own key, pointer, record injection, tombstone and removal, exactly as a single. Type 9 is handled before the whitelist log line (rule 12). The lay step leaves this loop.
7. **Lay pass**, after the loop and before `RemoveConnections`, when `layEdgeNetwork`:
   - singles: as today (`LayEdgePiece`, once per key per session on the timed path, always from the cheat);
   - pairs: when `cheat || !tried(lo) || !tried(hi)` and both lanes' `FindConnection` are non-null, run the pair lay and only then insert both keys into `edgeLayTried`. An unpaired lane or a lane whose partner is a tombstone is never laid and is not marked tried, so the pair is laid once it completes. Unpaired lanes log once per key.
   - pairs whose lanes are both new here (neither key known, `HasConnection` false for both) are laid in the pair pass, before the create loop, so `CreateConnection` places the markers on paved cells as the player's tool commit does (the avenue tool removes markers already present, Q12). Both keys are then marked tried, and a failed pre-create lay still creates both connections, arrow only.
8. **Pair lay** = `LayEdgePiece` generalised to one or two stubs (`lines[0]` is the placement line, hi; `LayEdgePiece` stays the n=1 wrapper so single-network log output is unchanged):
   - already laid: hi's edge cell holds a piece of the net → `already laid`, skip; only lo's does → log, skip (never lay over a half stub); in the already-laid branch (singles too) set the side byte when it is not 2, idempotent, no placement, so a re-created connection over surviving pieces gets its flag back. The re-set applies only to a piece with the 0x04 bit in its inward byte (`(f >> (((side+2)&3)*8)) & 0x04`); any other piece is logged and skipped, so a parallel avenue along the edge is never flagged;
   - survey the union of both 3-cell lines with the existing rule (cells holding any network are kept, the marker is kept, everything else is demolished), both rows because a building on lo's row would fail or half-lay the placement;
   - before placing, log the pieces of the net on the edge cells of rows lo-1 and hi+1 (the rows outside the block);
   - funds before and after, `DemolishRegion` on the block, `Init`, one `PlaceNetworkOccupants` over hi's line with net 2, 6 or 12;
   - verify: all 6 block cells with `IsOfType(net)`, and rows lo-1 and hi+1 unchanged. Lo's row empty or an outside row changed: log, skip the flags and the second placement, leave both connections (arrow only, as a failed single is left today). No fallback placement: a second call could only add lanes outside the demolished block;
   - edge flags: hi's edge cell as today; lo's edge cell only if hi's piece has its 0x04 bit toward lo or lo's piece has it toward hi (else log and skip: that piece is not hi's partner);
   - second `PlaceNetworkOccupants` over hi's line, verify again. `laid` means both edge cells hold a piece; a missing inner cell reports partial, as singles do.
   - Nothing in the lay path ever deletes a connection (a delete would queue an add=0 entry into the publisher's file and leave injected record copies active).
9. **Removal**: unchanged code, per lane, as vanilla's add=0. Known parity behaviours to record: removing one lane clears the surviving lane's side byte through the 0x04 partner step and nothing re-sets it until the next lay (rule 8 covers a re-listing); the one-cell re-solve uses the width-2 tool, so step 5 logs rows lo-1..hi+1 before and after each `EdgeClear`. An incomplete list makes lanes look unpaired: they are created with their dir, not marked tried, and the pair lays when the list completes.
10. The connection-created event `0x8A0D15DA` fires once per lane in both instances; the handler only sets the dirty bit.

### Ferry

11. Type 9 stays off the whitelist. Reason: our own scan creates a ferry for the same water run at load, deals never use ferry, traffic reads our own, and a mirrored one could never be removed. Nothing functional is lost.
12. When a publisher lists a type 9 that converts to our edge, log once per publisher session (`gateLogged` with a "ferry" marker, cleared by `DropHistory`) whether `GetConnectionsOfTypeToCity(9, A)` is empty here ("any ferry to A", coarse). First-time game call: cheat path only until observed.

## What must not break

- Singles: identical behaviour and log lines, except the flag re-set in the already-laid branch.
- Removal of one lane by the publisher: that lane only; the other stays until it is unlisted.
- Rebuild detection and tombstones per lane, unchanged.
- A pair split across a tile corner or straddling a third tile: unpaired, connection only.
- Demolition never touches more than the 2 by 3 block, never network cells or the marker.
- `make test` passes with the updated whitelist expectations.

## Accepted divergences (record in Q12)

- We demolish lo's row too; vanilla does hi's row only.
- We pair per (type, edge) instead of vanilla's single parity counter over the whole queue, so an odd lane elsewhere does not shift our pairs.
- An unpaired dual lane gets no stub (vanilla: same).
- Ferry not mirrored; our own terrain scan supplies it.

## Experiment (Q12)

Cheats first (`RegionSyncMirror`), then the timed path. Each step logs rows lo-1..hi+1.

1. A drags an avenue to a bare stretch of the shared edge. A's publish lists two type 8 entries on adjacent cells. B `RegionSyncMirror`: pair line with lo, hi, dirs and `driveOnRight`, two `CreateConnection`, two record injections, outside rows unchanged, 6 block cells verified, `laid`, 2 by 3 avenue stub under two arrows, funds unchanged. Compare the arrows with a vanilla-loaded avenue connection on the same edge. Run it in both directions (B's west and east edges) and once on a north or south edge pair (DumpsVille (4,4) above BumperVille (4,6) is not adjacent; use a tile pair that is).
2. B drags an avenue from the stub inward; a garbage deal becomes available over it.
3. Elevated highway (3) and ground highway (12): same as 1. For 3, note the raised marker and the elevated stub.
4. A house on lo's row: demolished, stub laid. B's own avenue already on the edge cells: `already laid`, flags re-set if needed, no demolition.
5. A bulldozes the whole stub: B's next pass removes both lanes (`removed=2`), two `EdgeClear` lines, arrows gone, no stray piece on the outside rows. A bulldozes one lane only (if the game allows it): B removes that lane; the survivor stays, its flag cleared (parity).
6. A ferry listed by A on B's edge: the ferry log line, nothing created.
7. Save both, reload both: stubs persist, no duplicates, `already laid` for the pair.
8. Timed path, no cheats: 1 and 5 again, including hi listed before lo in the snapshot (check the order in the file).

## Docs and build to amend

`PHASES.md` (split dual and ferry), `README.md` cheat row for `RegionSyncMirror` (no longer "single-network"), `ARCHITECTURE.md` source map (pairing helpers, pair lay), `PLAN-PHASE2.md` deferred items marked superseded (its "dir alternates" note is replaced by the formula), `OPEN-QUESTIONS.md` new Q12 with an empty Observed slot, `CLAUDE.md` status line, `src/SC4RegionSync.ini` comment for `LayEdgeNetwork` if it says single.
