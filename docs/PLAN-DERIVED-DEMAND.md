# Plan: sync the derived region-use demand keys (Q19)

Status: planned and reviewed 2026-10-22 (two adversarial reviews, no blockers), not yet implemented.

## Problem

The plugin carries, applies and feeds the 12 RCI group keys (`0x1010` to `0x4400`). The
demand simulator has 28 more region-use demands (workforce, census, jobs-per-group,
use-2 demand keys) that vanilla also sums over the connected closure into the
`0x1xxxx` targets at city Init. Between loads those stay at their load-time values, so
the demand side of job groups lags live peer growth while the supply side follows it
(`docs/OPEN-QUESTIONS.md`, "Demand check (2026-10-22)").

## Facts (Ghidra, 641 exe)

- `GetLocalPopulationSummary` `0x6a5f80` walks every demand in `demandSim+0x50` with
  region use 1 (stores `QuerySupplyValue`) or 2 (stores `QueryDemandValue`), rounded,
  into the record's `+0x3c` map. Not limited to the 12 groups.
- Init `0x6a8c20` calls the fold `0x6a5cb0` once (`0x6a8d9d`); the other caller is the
  connection handler `0x6a9694`. The fold covers every use-1 or use-2 demand and sets
  `demand[id+0x10000]` absolutely to the sum of slot 17 (`GetExtrapolatedPopulation`,
  `+0x48` else `+0x3c`) over `GetAllConnectedCities`. There is no separate load-time
  derivation.
- Region-use keys (property `0x47bb3f60`), 40 in total: the 12 groups; `0x1001`;
  `0x1011`, `0x1021`, `0x1031`; `0x2010`, `0x2020`, `0x2030` (use 2);
  `0x2111` to `0x2114`, `0x2121` to `0x2124`, `0x2131` to `0x2134`; `0x3111`, `0x3121`,
  `0x3131`, `0x3321`, `0x3331`; `0x4101`, `0x4201`, `0x4301`, `0x4401`. CAM overrides
  the same keys and adds none. `0x11001` is also derived by a `47bb3f50` sum link from
  `0x11010/20/30`, the only `0x1xxxx` key with two sources; it has no outgoing links, so
  it is left out of the carried set (39 keys). `0x14201` and `0x12010` have no link
  between them, so feeding both is what vanilla does.
- The derived estimators act only on group ids (`0x6a68e0` returns unless k is
  `0x1010/20/30`, `0x6a6af0` unless k is one of the nine C/I groups), inside the group's
  `n >= 10` gate, and call `ExtrapolateGrowth` on derived keys without checking those
  keys' caps. The outer loop of `0x6a8ff0` walks `this+0x3c` (vslot 0x88) with no
  region-use filter; whether it includes derived keys is unknown, and if it does each is
  gated by its own cap, which we pin.
- Monthly extrapolation `0x6a8ff0`: the derived estimators `0x6a68e0` (R: workforce
  split over `k+0x1101..k+0x1104` by record `+0x30`) and `0x6a6af0` (C/I: `k+1` jobs and
  `0x2010..30` demand) sit inside `if n >= 10` where `n = min(cap54 - ext48, unmet)`.
  A pinned cap (`FreezeExtrapolation`) gives n = 0, so they are frozen for every record
  we apply. They only estimate neighbors nobody is playing.

So for a live peer the real value of every derived key is already in its record after
`UpdateCityCache`; no formula is needed on the receiving side.

## Change

1. One key table, game-free so `make test` can include it (new `src/RegionalDemandKeys.h`
   or in `NeighborSnapshot.h`): `kRegionalDemandKeys`, 39 keys, the 12 groups first.
   `kRCIGroupIds` becomes a view of the first 12 (or stays, with `static_assert`s that
   the first 12 match, all keys are unique, and the count fits `kRCIGroupSlots`). A pure
   predicate `IsCarriedDemandKey(id, derived)` decides membership for apply and
   discover, so both sites follow one setting.
2. `src/NeighborSnapshot.h/.cpp`: `kRCIGroupSlots` 12 -> 64. File format and
   `schemaVersion` unchanged (`rciGroupCount`, `rci.i.groupId`, `rci.i.population`);
   the parser already clamps at the slot count (`NeighborSnapshot.cpp:355`), so an old
   DLL reads the first 12 (the groups) and a new DLL reading an old file sees 12 and
   moves nothing derived. The launcher treats snapshots as opaque (256 KiB cap; 39
   entries add about 1.3 KB).
3. `CaptureRegionalCity`: loop over `kRegionalDemandKeys` with `GetPopulation(k)`
   (after `UpdateCityCache`, so it reads what vanilla would save). Capture always writes
   all 39; `GetPopulation` of a missing node reads 0, which the receiver must not write
   for a key without a node (it never inserts, so this holds by construction).
4. `ApplySnapshotToRegionalCity`: apply only ids for which `IsCarriedDemandKey` is true
   (hostile or other-plugin ids from LAN peers are ignored). Pin `+0x48` and `+0x54`
   independently of `+0x3c`: today the loop `continue`s when the `+0x3c` node is
   missing, which would leave an existing `+0x48`/`+0x54` unpinned; a missing cap node is
   harmless (operator[] inserts 0, allowable 0). Count skips per map and log the counts
   once per pull at Info.
5. Director `DiscoverRegionalDemand`: iterate `kRegionalDemandKeys` through the same
   predicate, keep the filter on index-0 `GetRegionUse()` 1 or 2, target at index
   `0x10000`. The load-check line compares `QuerySupplyValue` for use 1 and
   `QueryDemandValue` for use 2 (the fold sets the target's demand for `0x2010..30`).
   `SumOtherTiles`, `SumConnectedTiles`, `FeedRegionalDelta` and the connection
   rebaseline then cover all keys unchanged; use-2 keys already go to
   `AddToDemandValue`. Index 0 is never written.
6. `SyncSettings` and `SC4RegionSync.ini`: `SyncDerivedDemand`, default 0 in the first
   build (27 new game objects and record nodes), comment written like
   `FreezeExtrapolation`'s, logged with the other settings. Flip to 1 in a separate
   commit with the Q19 Observed entry.
7. Logging at Info: the load-check line per key, the `Apply: rci` lines, the feed lines,
   the per-pull skip counts, and a line marking the first pull after load (its write-back
   of saved `+0x48` values is a one-off correction).
8. Tests (`make test`): 39-entry round trip (order, ids, values); `rciGroupCount=100`
   with 100 entries clamps to 64 with no overrun; a 12-entry file parses with count 12;
   negative and huge derived values hit the existing clamp; key-table invariants;
   `IsCarriedDemandKey` with the flag on and off.
9. Docs: ARCHITECTURE.md ("12 groups" at lines 91 and 145, source map), PHASES.md
   checkbox, OPEN-QUESTIONS.md Q19, DESIGN.md one sentence (derived demand keys are
   carried; "what stays local" lists only traffic, thumbnails and the sim clock, so no
   contradiction), README if it names demand sync, CLAUDE.md status line.

## Q19 experiment

1. Flag off: for every derived key the load line shows target supply = sumConnected.
2. Flag on, wiped store: grow a peer's R$ by about 1,000; the receiver's `0x12111` to
   `0x12114` and `0x11011` targets move by the peer's record delta on the next pull.
3. Same with a peer's industry: `0x14201` and `0x12010` to `0x12030` (demand side).
4. Soak three game years: no saw-tooth, no target below 0.
5. Save the receiver, reload, redo step 1.

6. Mixed builds, manual: new publisher with an old receiver and the reverse; derived
   keys must change nothing.

Exit: step 1 holds for all keys (use-2 keys compared on demand); from the second pull
on, each per-pull change equals the peer's published change on every key whose node
exists (the first pull may carry a one-off correction of saved extrapolation, logged);
no growth between pulls; after reload the load check equals the live value before the
save; skip counts stay reported.

## Must not break

- Q16: cap pinned for every carried key (also freezes the derived extrapolation).
- The 12 group publish values stay identical.
- Saves: the plugin writes none. As with Q16, a save persists the pinned caps, and Q4
  neighbor writes carry them into peers' `.sc4` files; this now covers 39 keys, so
  vanilla extrapolation stays off for those keys in a later vanilla session until the
  tile is played and saved again. Same trade as Q16, wider.
- `make test`, `make test-launcher`, LAN mirroring (snapshots are opaque files to it).

## Operator steps

`make`, `make test`, `make deploy` (game and launcher closed), wipe the store with every
instance and launcher closed (on a LAN pair, both PCs, or the mirror restores the old
snapshots; the wipe is only for a clean step 1, the first publish overwrites anyway),
same build on both PCs (the LAN build check should refuse a mismatch), start the
instances, run the steps, `make logs`, fill the Q19 Observed slot.

## Unknowns

- A peer founded mid-session whose receiver record lacks derived nodes is skipped
  (never insert); the skip count shows it.
- Region-use keys added by other plugins are not carried.
