# Regional population: what the record holds and where the fold lands (SC4 1.1.641)

Static analysis only. Raw dumps: `connections/pop*.txt`.

## Facts

**UpdateCityCache(city) `0x4ad290`:**
- `+0x24` population = `city->GetResidentialSimulator()` (city slot 68) `->GetPopulation()` (slot 16).
- `+0x3c` per-type map = `city->GetDemandSimulator()` (slot 56) `->GetLocalPopulationSummary(map)` (slot 11, `0x6a5f80`). That function clears the map, walks the demand map at `demandSim+0x50` and, for each demand with `GetRegionUse()==1`, stores `(int)QuerySupplyValue()` under its key (use 2: `QueryDemandValue()`). Only the region-use objects (index 0, key `0x1010` etc.) contribute.
- `+0x28/+0x2c` jobs = sums over 0x31x1/0x33x1 and 0x4x01 of `CalculateJobsPerUnitOfDemand` (slot 8) times the record's `GetPopulation(type)`.
- It never touches `+0x48` or `+0x54`. Write `0x4a7d10` and Read `0x4ac5b0` serialize all three maps (`+0x3c` plain, `+0x48` extrapolated, `+0x54` extrapolation cap). Read only loads them.

**Fold `0x6a5cb0(bool absolute)`** (Init `0x6a8d9d` passes 1; connection messages `0x6a9694` pass 0):
- New list = `neighborsSim->GetAllConnectedCities(list, false)` (slot 22, `0x6d1ab0`). That is the transitive closure over connections of types 1,2,3 and 6 to 12, with unestablished tiles dropped (record slot 43, byte `+0x168`). It also refreshes `GetConnectedCities(+0x9c, false)` (slot 21, direct connections only). The old list is `+0x98`. The own record is excluded.
- For each region-use demand D (key k): **the target is T = demand[D.GetId() + 0x10000]**, i.e. `GetDemand(k, 0x10000)`, not D. The sum is `GetExtrapolatedPopulation(k)` (slot 17) with no scaling.
- `absolute=1`: `T.AddToSupplyValue(newSum - (T.QuerySupplyValue + T.QueryNewSupply))`, which sets T to the sum. `absolute=0`: adds `newSum - oldSum`. Use 2 goes to `AddToDemandValue` the same way. The own record is never adjusted.

**Monthly extrapolation `0x6a8ff0`** (SimNewMonth `0x66956816` → `0x6a9450`, gated per type at more than 65 days):
- For each type whose regional demand is unmet (demand minus supply ≥ 10.0), it walks the **directly connected** list `+0x9c`, ordered by a per-type weight (record slot 16 × 3.0).
- For each neighbor in turn: n = `min(GetAllowableExtrapolation(k), unmet)`, capped at 1000. `GetAllowableExtrapolation` (`0x4a9820`) is `max(0, cap54[k] - ext48[k])`.
- If n ≥ 10: `rec->ExtrapolateGrowth(k, n)` (`0x4a9860`, `ext48[k] += n` via operator[], which inserts 0 when the key is missing), then `demand[k+0x10000].AddToSupplyValue(n)`. Then `0x6a68e0` (R types) and `0x6a6af0` (C/I types) extrapolate the derived job/commute types the same way, from the neighbor's `GetPopulation(k)`, `GetPopulation(k+1)` and `+0x30`.
- This is the "extrapolated N" drift. The game adds the same n to the 0x10000 object, so it is self-consistent in vanilla.

## Why the values go negative

The plugin feeds `ApplyRegionalDemandDelta` into the **index-0** objects. Those are exactly what `GetLocalPopulationSummary` copies into `+0x3c`. In vanilla, index-0 supply never contains regional population, because the fold goes into 0x10000.

So in index-0 supply, `local + (lastSum - baseline_at_load)` comes out as `local - baseline`. Then `SubtractRegionalShare` subtracts the whole `lastSum`, a baseline that was never in there. The receiver feeds the resulting negative change into its own index-0 supply, which explains BumperVille's R$ supply of -3280, and that comes back on its next publish. A coupled loop.

Two other leaks add to it. `SumOtherTiles` sums all established tiles, not the connected closure. And the game's own `ExtrapolateGrowth` raises `+0x48`, which the next pull's `lastSum` delta counts a second time (the game already added n to T) or reverses when the pull overwrites `+0x48`.

The earlier "18k → 91k" loop had the same cause. The vanilla record is not contaminated: it equals local supply.

## Recommended scheme (the simpler one: keep supply, fix the target)

1. **Publish** the record value as the game wrote it (`UpdateCityCache`, then `+0x3c`). Delete `SubtractRegionalShare`. No census or residential-simulator read is needed: `+0x3c` is by definition what vanilla saves for the tile.
2. **Feed** regional changes into `GetDemand(group, 0x10000)` (the object the fold and the extrapolation already use), never into index 0. In `DiscoverRegionalDemand`, keep the index-0 object only to learn the region use, and store the 0x10000 object as the target.
3. **Delta per pull** = `S_after - S_before`, both measured around the record writes in the same call, where S is the sum of `GetExtrapolatedPopulation(k)` over `GetAllConnectedCities(list,false)` (call the interface; it excludes self). Do not carry `lastSum` across months: the game's own extrapolation between pulls already went into T, and a before/after pair absorbs it. The existing connection-message rebaseline stays correct.
4. **Receiving record**: write `+0x3c` = published value. Write `+0x48` = published value too (insert-free, as now; if the node is missing, slot 17 already falls back to `+0x3c`). Leave `+0x54` alone. A later vanilla load then sees a plain local-supply record, with extrapolation restarting from the real figure. Clamp to ≥ 0 as a guard.
5. Sessions that already ran the old feed have contaminated index-0 supply. Relaunch both instances with a wiped store.

## Unknowns / checks

- That `GetDemand(g, 0x10000)` exists for all 12 groups was not checked statically (the fold skips missing keys). **Experiment:** at city load, log `GetDemand(g,0x10000)->QuerySupplyValue()`. It should equal the connected-closure sum of slot 17, because Init sets it absolutely. That one line confirms the whole model.
- Who first writes `+0x54` (cap) and seeds `+0x48`. No writer was found besides Read and `ExtrapolateGrowth`; probably the region-side save path (Q4). Writing `+0x48` below the cap invites the game to extrapolate up to 1000 per type per neighbor each cycle. Step 3 accounts for that, but the cancel shows on the demand graph as a saw-tooth (Q16, 2026-10-18); the apply now pins `+0x54` to the live value as well (`FreezeExtrapolation`).
- What connection types 4 and 5, excluded from the closure, mean.
- Whether `AddToSupplyValue` on the 0x10000 object moves the RCI graph as visibly as index 0 did. It should, since vanilla's whole regional effect goes through it.
