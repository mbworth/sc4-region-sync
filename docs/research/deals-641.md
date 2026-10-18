# Neighbor deals in SC4 1.1.641: storage, propagation, live injection

Ghidra sources: a copy of `ingame_tests/ghidra`, plus scratch scripts in `deals/scripts/`. Cross-checked against tloriato/OpenSC4 `docs/specs/neighbors.md` and `docs/abi/allocators.md`, which agree on every point I checked. No Null45 or NAM code calls the deal APIs (GitHub code search).

## 1. Deal object (`cSC4NeighborDeal`)

- Class `0x2A134F42`, IID `cISC4NeighborDeal` = `0x0773FEF1`. The factory `0x006CF940` (new 0x80 bytes, ctor `0x006CF270`) is registered with GZCOM at `0x00460A7F`, so **`CreateInstance(0x2A134F42, 0x0773FEF1)` works**. `ProposeDeal` and `Clone` also create deals. Vtable `0x00AB1DD8` (41 slots) matches the header order. Serializable at +4, property holder at +8.
- Layout: +0x20 type, +0x24/+0x28 other-side cell, +0x2C/+0x30 city-side cell, +0x34 amount, +0x38 incAmount, +0x3C incAmount2, +0x40 lastAmount, +0x44 lastIncAmount, +0x48 amountUsed, +0x50 cost (s64), dates (Julian day) at +0x58/+0x60/+0x68/+0x70, +0x78 flags. Initial flags are `0x81`.
- Types (+0x20). Even = **buy (import)**, odd = **sell (export)**: power `0x4773FFE0`/`E1`, water `E2`/`E3`, garbage `E4`/`E5`. Evidence: expense sums E0/E2/E4 (`0x006CF8D0`), income sums E1/E3/E5 (`0x006CF860`).
- Flags (`SetFlags` `0x006CFA50`, `Activate` `0x006CE640`, `Cancel` `0x006CE610`, state machine `0x006D0E50`):
  - `0x1` Proposed, `0x2` Active, `0x4` Cancelled
  - `0x8` NoConnection, `0x10` CannotAfford, `0x20` NoLandfill (garbage sell)
  - `0x40` Expired/renewal due, `0x80` **Silent** (no status messages, not simulated)
  - `0x100` Grace, `0x200` Probation
  - While Grace is set, `SetFlags(on)` drops bits `0x38`.
- **The flags do not say which side initiated.** Every deal is stored from its owner's point of view: the city side is "us".
- `Clone(&out, silent)` (`0x006CF3E0`): Copy, then `SetFlags(0x80, silent)`. If the city-side tile is not `App->GetRegionalCity()`, it **swaps the cells and toggles the type** buy<->sell. If either tile is missing, the clone is marked Cancelled.
- `SetAmount` (`0x006CF960`): ignored while 0x300 is set. Otherwise it stores the amount, **sends** status 0 synchronously, then calls `UpdateCost`, which sets cost = `GetDealCost(type, amount, otherCity)` with a minimum of 1.
- `UpdateState` (`0x006CF360`):
  - Buy type: sets 0x10 if own budget < cost.
  - Otherwise: sets 0x8 if the other city is missing or `FindConnectionSuitableForDeal(type, other)` returns null.

## 2. Storage

- The neighbors simulator keeps the live list at **sim+0x20**. Its save record (`Write` `0x006CFB60`) holds **connections only, not deals**.
- Each regional record has two deal lists, both written by `cSC4RegionalCity::Write` (`0x004A7D10`):
  - +0x18C: deals loaded from disk
  - +0x190: "local deals"
  - `Read` (`0x004AC5B0`) loads both into +0x18C, marked Silent.
- Corrected slots: 68 `UpdateLocalDeals` (`0x004A9110`), 69 `SetLocalDeals` (`0x004A90D0`), 70 `GetLocalDeals` (`0x004AA0C0`, copies +0x190), 71 `UpdateImportExport` (`0x004AA200`).
- The filter (`0x004A8EC0`, thiscall on record R) keeps only deals between **R and the loaded city**.
  - `UpdateLocalDeals` moves those from +0x18C to +0x190.
  - `SetLocalDeals(list)` clears +0x190, copies the matching deals from `list` (references to the sim's own objects, loaded city's view), then calls `UpdateImportExport`.
- **Proposing in A writes nothing special onto B's record.** A's sim keeps the deal. Pushes (`AddDeal`, ticks) call `SetLocalDeals` on every adjacent record and on A's own, still in A's view.
- **Saving A writes only A.sc4.** Its record carries the deal in +0x190.
- **B learns of the deal only in its sim `Init` (`0x006D38F0`)**:
  1. `ReloadCity` on each adjacent tile. This **replaces the record objects**, so pointers taken earlier go stale.
  2. Own `UpdateLocalDeals`.
  3. For each neighbor N: `N.UpdateLocalDeals`, then `GetLocalDeals`. Each deal is `Clone`d (mirrored to B's view), un-Silenced, appended to sim+0x20, and its dates are shifted so that currentDate = today.
  4. Push.
- **There is no offer prompt in B.** The offer dialog is A-only UI: `ProposeDeals` `0x006D21A0` evaluates B's record (budget, `GetDealMaxAmount`, `GetDealCost`), and the player accepts in A. A proposed deal becomes Active on A's next deal-simulation run. B just finds the deal when it loads. The advisor (`cSC4AdviceNeghborDeal`) reacts to status reports (activated or terminated).

## 3. Live injection into B

**What the two calls do:**

- `MarkDealsDirty` (slot 31, `0x006CFD70`) only sets `sim+0x3C = 1` ("deals dirty"). On the next SimNewDay `0x66956814` or SimIdle `0x6695681A`, `FUN_006D0A40` re-pushes **B's own** deal list onto the adjacent records and B's own (`SetLocalDeals`), plus `UpdateImportExport` if stats are dirty (+0x3D).
  - It does not import anything, and it overwrites whatever is in A's record +0x190 in B's process.
  - Keep calling it or not; it is harmless.
- `UpdateLocalDeals` only moves disk-loaded deals (+0x18C to +0x190) for the loaded city. It is useful only right after a `ReloadCity`.

**Recommended sequence (all public calls; run after ConnectionsReady `0x6AC284F3`, i.e. post city-init; wrap in try/catch and log at Info):**

1. Map A's deal to B's view: `tB = tA ^ 1`, other city = A's record (found by A's cell).
2. Dedupe: `FindDeals(tB, pA, list)` on B's sim. **The list holds `cISC4NeighborDeal*`, not connections; the vendored header's element type is wrong.** If a deal exists, go to step 6.
3. `ProposeDeal(tB, pA, &d)` (slot 24). It returns false when B has no suitable connection. Treat that as "cannot mirror, log". This validates and fills the cells correctly; no `Clone` or raw writes are needed. `d` is Silent + Proposed.
4. Dates, while still Silent: `GetSimDate()->Clone(&t)`, then `SetCurrentDate(t)`, `SetStartDate(t)`, and `t += (A.exp - A.cur)`, `SetExpirationDate(t)`. Use the uniquely named `Clone`, `operator+=` and `DayNumber`, and avoid the overloaded `cIGZDate::Set`.
5. `AddDeal(d)` (slot 26, `0x006D2870`): un-Silences, appends, pushes to all adjacent records plus own, runs `UpdateImportExport`. Then `d->SetAmount(A.amount)`, which recomputes cost and synchronously runs the deal simulation. That run activates it (status 1), and B's budget line appears via `GetTotalMonthlyNeighborDeal*`.
6. Existing deal: if the amount differs, `SetAmount`. If A cancelled or removed it, `Cancel()`; the sync status 0 run removes it (status 2) and re-pushes.
7. `Release` everything you hold.

**Alternative that mirrors `Init` exactly:** `CreateInstance`, then set type, cells, amount and dates in **A's** view while Silent, then `Clone(&e, true)` (mirrors), `AddDeal(e)`. This needs `SetAmount` before any 0x300 flags. Cost is copied, not recomputed, until renewal.

## 4. Cancellation, amounts, import/export stats

- Each instance runs its own deal simulation on its own copy, on SimNewMonth and on every `0xEA418D8F`. Each books only its own side: expense for buy, income for sell, both excluding flags `0x3D`. So two live copies do not double-bill.
- Cancel in A reaches B only through the sync: B calls `Cancel()`. Failures go through 90-day Grace and 90-day Probation before removal (tuning `0x6B588FAD` / `0x8B58C59E`), separately on each side.
- Amount: set by the player through `SetAmount`. Sync only `amount`. Cost is derived locally (`UpdateCost`). `amountUsed`, `incAmount` and `lastAmount` are maintained by each side's utility simulators; do not sync them.
- **Imported and Exported are derived.** `UpdateImportExport` zeroes them for commodities 1..3, then sums the `amount` of each deal on the record's +0x18C and +0x190 lists, skipping flags `0xBC` (`0x004AA0E0`). The sums go into the commodity node: +0x10 Exported, +0x18 Imported. Also:
  - `UpdateCommodityStats` (`0x006D2A20`) rewrites the own record's stats from the city's simulators.
  - Writing the stats from a snapshot is therefore overwritten on the next push. That is exactly the zeroing we observed.
  - Inject deals instead, and the stats follow.

## 5. Messages

- `0xEA418D8F` deal status: posted (sent synchronously only by `SetAmount`), IGZUnknown = deal (QI `0x0773FEF1`), data1 = reason:
  - 0 changed
  - 1 activated
  - 2 cancelled/removed
  - 3/4/5 removed for NoConnection/CannotAfford/NoLandfill
  - 6 expired
  - 7 renewed
  - 8 retry, no deal attached
- No message is sent for Silent deals. A's plugin should subscribe, set a dirty bit and publish on the next tick, debounced; there are bursts of status 0. Do not publish from inside the handler, because sync sends re-enter.
- The sim also handles CityEstablished `0x26D31EC4` and CityObliterated `0x26D31EC5` (clears its list and pushes), ConnectionsReady `0x6AC284F3`, and SimNewMonth/Day/Idle.

## 6. Minimal snapshot record per deal (publisher's view)

- `type` (u32)
- `cityCell` x,z (the publisher) and `otherCell` x,z. Identity = (unordered cell pair, commodity = (type-0x4773FFE0)/2).
- `amount` (s32)
- `flags` (u32, mask out 0x80)
- `cost` (s64): diagnostics only
- Dates as offsets from the publisher's sim today: `cur-start`, `exp-cur`, `susp-cur` (only meaningful with 0x100/0x200)
- `rev`/origin tag, for echo suppression

## SC4List check

The layout is correct for these calls. The head pointer goes to a 12-byte root; nodes are `{next, prev, value}`; the allocator is the GZ allocator service (`0x3AE4BEA3`/`0x3AE4BEAB`), the same one `SC4List.h` uses. Evidence: `AddDeal`, `FindDeals` and the clear at `0x00421660`. `GetAdjacentCities` clears the list and AddRefs each element.

The earlier crash is more likely one of these:
- (a) calling header slot 9 `GetPopulation()` on the elements: the binary has `RET 4` (it takes a type), so the stack is unbalanced;
- (b) using a record after `ReloadCity` replaced it.

`src/RegionSyncDllDirector.cpp:839` still calls `pAdj->GetPopulation()` through the header.

## Risks and unknowns

- Not yet run in game. Run step 3 on a cheat first.
- **Echo loop:** B's `AddDeal`/`SetAmount` fire status reports that B's plugin would publish back to A. Compare before applying, and tag the origin.
- Two copies can diverge: different Grace timing, or one side NoConnection. Pick a policy, for example "the seller's copy is authoritative for existence, and amount comes from whoever changed it last".
- The `ProposeDeal` path needs B to have the matching connection (power pole/pipe; garbage needs a road-type link). Vanilla has the same rule.
- New A-to-C deals do not appear on B's copy of A's record: there is no public setter for +0x18C. B's demand and capacity reasoning about A sees only deals A has with B.
- If B saves, B.sc4 contains the mirrored deal, and A's next cold load clones it again. Dedupe via `FindDeals` on load as well.
- The `cIGZDate` overload order is unverified, so stick to the uniquely named methods.
- `GetDealCost` is evaluated in each process with that process's view of the neighbor's record, so A's cost and B's cost can differ slightly.
