# sc4-region-sync: design

## The problem

SimCity 4 simulates exactly one city tile per process. A tile only learns about its neighbors when it loads: the game reads each adjacent tile's save file into an in-memory summary record (`cISC4RegionalCity`) and the demand, neighbors, power, water and traffic simulators read those records for the rest of the session. The player has to leave a city, enter the neighbor, let it run, save, and come back for the region to "catch up". That round trip is the annoyance this project removes.

## The approach

Run one game instance per city, on separate cores, and keep the per-tile summary records in every instance current with what the other instances are actually doing. Nothing heavy crosses the border. A tile's regional record is population, jobs, per-commodity import/export/production/demand numbers, budget, and the neighbor deals attached to it. That is kilobytes, and it is the only thing the game itself ever moves between tiles.

Each instance carries the same DLL plugin. The plugin has two jobs:

1. **Publish.** Refresh this tile's own regional record from the live city (`cISC4RegionalCity::UpdateCityCache(cISC4City*)`, the same call the game makes on save), capture it into a plain `NeighborSnapshot`, and write it to a shared store.
2. **Pull.** Read every other tile's snapshot from the store and write the values into this instance's regional record for that tile (`cISC4RegionalCity::Set*`), then `cISC4NeighborsSimulator::MarkDealsDirty()` so deal state is recomputed.

Both run on a sim-month cadence and on demand from the cheat console.

## Why the regional record is the right seam

Everything that makes a neighbor "matter" to this tile goes through `cISC4RegionalCity`:

- **Regional demand.** The demand simulator looks at regional population and jobs for the RCI caps. Those come from the regional records.
- **Neighbor deals.** Power, water and garbage deals are `cISC4NeighborDeal` objects hanging off the regional records, with amounts, costs and expiry dates. The neighbors simulator recomputes them from the records (`FindDeals`, `GetDealMaxAmount`, `GetDealCost`).
- **Connections.** `cISC4NeighborConnection` objects know which regional city is on the other side and act as traffic sinks/sources. They are created from the road network, not from the neighbor, so they do not need to sync; only the record they point to does.

We never touch the simulation of the other tile. We update the summary it would have written on save, continuously instead of on save.

## What stays local

- **Traffic across the border.** Commuters leaving this tile are consumed by the connection as a sink with a capacity derived from the neighbor's record. They never arrive as agents in the other instance. This is how the game has always worked; we only make the capacity number fresh.
- **Region view thumbnails and terrain.** Each instance's region view shows what it loaded. Not worth syncing.
- **Sim clock.** Tiles drift apart in date. The game already tolerates that; we do not try to lockstep instances.

## The store

Phase 1 uses the filesystem: one `<x>_<z>.snapshot` key=value file per tile under `<Plugins>\RegionSync\<region folder>\`. Writes are atomic (temp file plus rename) so a reader never sees a torn snapshot. Both instances share one install, so the Plugins folder is already shared storage with no setup.

Phase 2 replaces the file poll with a named pipe or shared memory and a tiny coordinator, only if file polling on a monthly cadence turns out to be too slow or too coarse. It probably is not: a sim month at cheetah is seconds, and a directory scan of a dozen small files is microseconds.

## Save contention

Saving a city writes this tile's `.sc4` file and also writes updated connection and deal info into the adjacent tiles' files. With two instances saving against each other that is a corruption risk.

Phase 1 sidesteps it: autosave is off by default and the snapshot store carries the cross-tile data instead of the save files. The player saves each instance by hand as usual. Saves are serialized by a region-wide named mutex (`Local\SC4RegionSync.SaveLock.<region>`) held across the game's whole save routine: the plugin wraps App vtable slot 15 (`SaveCity(path, fast)`, which every save path reaches), so a save waits for the other instance's save or reload to finish, up to `SaveLockTimeoutMs`, then goes ahead unlocked and logs it. The kernel abandons the mutex if an instance dies, so there is no stale lock. See PLAN-SAVE-LOCK.md. See OPEN-QUESTIONS.md, Q4, for the open question of whether the neighbor-file write on save can be suppressed entirely while our store is active.

## What is still a guess

See OPEN-QUESTIONS.md. The big one: whether the demand simulator reads the headline population and jobs values from the regional record on each tick, or copies them once at city load. If it copies once, a pull will not move demand until the next city load and Phase 1b (`cISC4Region::ReloadCity`) or a deeper hook is needed. The `RegionSyncDump` cheat before and after a pull is the experiment that answers this.
