# Open questions

Each of these has a concrete experiment attached. Run the experiment, record the answer here, and the design in DESIGN.md either holds or gets adjusted. Keep this file honest: write down what was actually observed, not what we hoped.

## Q1. Does `cISC4RegionalCity::UpdateCityCache(cISC4City*)` have side effects beyond the in-memory record?

The game calls it on save. We call it before every publish. If it also writes to disk, pops a dialog, or touches neighbor records, we need to know.

**Experiment.** Load a city. `RegionSyncDump`. `RegionSyncPublish` (which calls it). `RegionSyncDump` again. Watch the save file's timestamp, the region folder, and the game for any UI change. Compare the two dumps.

**Expected.** Own record's pop/jobs change to match the live city; nothing else. If a file changed, set `RefreshCacheBeforePublish=0` and reassess.

**Observed.** (unknown)

## Q2. Does the demand simulator re-read regional records every tick, or cache them at city load?

This decides whether a pull moves regional demand immediately. Note that `cISC4RegionalCity` has no public setter for the headline population/jobs numbers, only for budget, income and the commodity stats, so even if the simulator does re-read, Phase 1 cannot yet push population into a neighbor's record. The RCI-group populations come from `GetPopulation(groupId)` and have no setter either.

**Experiment.** Two instances, tiles A and B adjacent and both established. In A, open the RCI demand graph and note the regional R/C/I caps. In B, grow the city a lot (or cheat money and zone heavily), let it run a few months so B publishes. In A, `RegionSyncPull`, then `RegionSyncDump`, and look at the demand graph. Did B's new numbers show up in A's record for B (yes if the dump shows them)? Did A's demand caps move (yes if the simulator re-reads)?

**If the record updates but demand does not move,** the simulator caches at load. Options: (a) Phase 1b, `cISC4Region::ReloadCity` on the neighbor's slot, then see if the demand sim notices a new record pointer; (b) find the simulator's cached copy and refresh it (`cISC4DemandSimulator` has a declared interface in gzcom-dll worth reading next).

**If the record does not update at all,** we need the population setter. The regional record is a concrete class in the game binary; the vtable has setters for other fields, and the population fields are almost certainly plain members. Finding their offsets from the getters is a small Ghidra task.

**Observed.** (unknown)

## Q3. Does `cISC4Region::ReloadCity` work mid-game, and what does it do to pointers?

If it works, Phase 1b is possible with no snapshot store at all: instance B saves, instance A reloads B's slot from the save file. The risk is that reload replaces the `cISC4RegionalCity` object and leaves the neighbors simulator and existing connections holding a stale pointer.

**Experiment.** In A, `RegionSyncDump` (note the "Reload ... recordPtrChanged" output from a `RegionSyncReload`). In B, change something visible (rename the city), save. In A, `RegionSyncReload`, then `RegionSyncDump`. Did the name change? Did `recordPtrChanged` report 1? Do deals and connections still resolve the other city's name correctly? Does the game stay stable for a few minutes afterwards?

**Observed.** (unknown)

## Q4. What exactly does saving write into neighbor tiles' files, and can it be suppressed?

Needed before autosave can be turned on with two instances. Expected: deal records and connection records go into the neighbor's `.sc4` so the neighbor sees the deal when it loads.

**Experiment.** Save a city; compare neighbor `.sc4` timestamps and sizes before and after. If they change, diff the DBPF index (SC4 save files are DBPF; any DBPF tool lists entries) to see which subfile changed.

**Follow-up.** If the writes matter, the clean fix is to route those through the store too; the quick fix is the save lock described in DESIGN.md.

**Observed.** (unknown)

## Q5. Does version 641 allow two instances at once, and does `-UserDir` work as documented?

**Experiment.** Run `scripts/Setup-MultiInstance.ps1 -Setup`, then `-Launch`. If the second instance refuses to start or immediately exits, look for a named mutex (Process Explorer, Handles view, search "SimCity") and patch it out in the plugin (`Patcher.h` in sc4-dll-utilities exists for exactly this kind of thing). Check that both instances see the same region and that each writes its log to its own place.

**Observed.** (unknown)

## Q6. Which commodity index means what?

`GetExported(int32_t nCommodity)` and friends take a small integer. Power, water, garbage are the deal types; the ordering is unknown.

**Experiment.** `RegionSyncDump` in a city with one active power deal and no other deals; whichever commodity slot has non-zero exported/imported is power. Repeat for water and garbage.

**Observed.** (unknown)

## Q7. Does the month message fire while paused, and does the game's own save trigger `PostCityShutdown`?

Affects cadence and the "publish on shutdown" path. Low risk either way, but worth confirming from the log.

**Observed.** (unknown)
