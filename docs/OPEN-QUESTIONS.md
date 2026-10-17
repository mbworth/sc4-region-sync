# Open questions

Each of these has a concrete experiment attached. Run the experiment, record the answer here, and the design in DESIGN.md either holds or gets adjusted. Keep this file honest: write down what was actually observed, not what we hoped.

## Q1. Does `cISC4RegionalCity::UpdateCityCache(cISC4City*)` have side effects beyond the in-memory record?

The game calls it on save. We call it before every publish. If it also writes to disk, pops a dialog, or touches neighbor records, we need to know.

**Experiment.** Load a city. `RegionSyncDump`. `RegionSyncPublish` (which calls it). `RegionSyncDump` again. Watch the save file's timestamp, the region folder, and the game for any UI change. Compare the two dumps.

**Expected.** Own record's pop/jobs change to match the live city; nothing else. If a file changed, set `RefreshCacheBeforePublish=0` and reassess.

**Observed (partial, 2026-10-17, single instance).** The in-memory refresh works: the monthly publish shows live values moving (`pop=11328` at load, `11942` ten sim-months later, no save in between), and the dump's own-tile row matched the latest publish. No dialog or UI change. Whether the save file or region folder changed was not checked; still open.

```
Publish (city load): (4,6) 'BumperVille' pop=11328 com=768 ind=3817 simDate=2055-01-20
Publish (month tick): (4,6) 'BumperVille' pop=11942 com=937 ind=3878 simDate=2055-10-01
```

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

**Observed (2026-10-17, Steam build): yes, with two fixes.** A second launch never started: it focused the first window and exited before plugins loaded (no Instance2 log). Two guards, both confirmed by removing them one at a time:

1. The game holds a Mutant named `SimCity 4 Mutex`. `RegionSyncUnlock` closes it from inside the running instance (a second Unlock no longer lists it). Closing it alone was not enough.
2. The Steam build hands itself to Steam at startup (the running process's command line has the Steam launch options appended), and Steam refuses a second copy of a running app. A `steam_appid.txt` containing `24780` next to the exe stops the relaunch. With both in place the second instance came up.

`ReleaseInstanceMutexOnStart=1` is now the shipped INI default and `Setup-MultiInstance.ps1 -Setup` writes the appid file. No ill effect from closing the mutex seen so far; watch for it.

```
Unlock (cheat): mutex handle 0x03BC '\Sessions\1\BaseNamedObjects\SimCity 4 Mutex'
Unlock (cheat): closed 1 handle(s) named 'SimCity 4 Mutex'.
```

`-UserDir` works: the log and the store path resolve under `SimCity 4 Instances\Instance1\` (store through the Plugins junction, so still shared).

## Q6. Which commodity index means what?

`GetExported(int32_t nCommodity)` and friends take a small integer. Power, water, garbage are the deal types; the ordering is unknown.

**Experiment.** `RegionSyncDump` in a city with one active power deal and no other deals; whichever commodity slot has non-zero exported/imported is power. Repeat for water and garbage.

**Observed (partial, 2026-10-17).** Only slots 1, 2 and 3 are populated; 0 and 4..7 are always zero. No deals existed, so the mapping is a hypothesis from magnitudes: 1 = power, 2 = water, 3 = garbage. Needs one deal of each type to confirm.

```
  tile (4,6) size=1 'BumperVille' est=1 ... pop=11914 com=922 ind=3873
    commodity[1] exp=0.00 imp=0.00 prod=7439.00 dem=5455.00 cost=0.0418
    commodity[2] exp=0.00 imp=0.00 prod=3557.00 dem=13174.00 cost=0.0512
    commodity[3] exp=0.00 imp=0.00 prod=7368.67 dem=22984.00 cost=0.0121
```

**Crash (2026-10-17).** Every `cISC4RegionalCity` setter we tried crashes the game sooner or later, on another tile's record (`SetBudget`, `SetProduced(2, ...)` during the city-load pull) and on the own record too. `RegionSyncSetterTest` on BumperVille's own record: `SetProduced(2, 3559)` returns but `GetProduced(2)` then reads `0.00`; `SetBudget(1015857)` returns but `GetBudget` reads `0`; the game crashed a few calls later. The getters at every slot return correct values, so the vtable order is right. Confirmed in Ghidra on the 641 exe (vtable at `0x00a8fb20`, found through `kcSC4RegionalCity` = `0x6A037C24` and the `QueryInterface` that compares `0x69D2BE1D`; scripts in `tools/ghidra/`):

- Slots 50..63: `GetBudget`/`GetIncome` do `FLD QWORD [this+0x170/0x178]`, `SetBudget`/`SetIncome` are `(double)` with `RET 8`; the commodity getters return `double` from a per-id map node and the setters are `(int32, double)` with `RET 12` (negative values clamped to 0). The header says `float` everywhere, so every setter call left the stack 4 bytes short: garbage stores, crash a few calls later.
- Slot 9 takes a `uint32` type (the cache-update routine calls it with `0x4401`, and `GetExtrapolatedPopulation` falls back to it), so it is `GetPopulation(type)`. The headline getters are one slot later than the header: 10 pop `[+0x24]`, 11 com `[+0x28]`, 12 ind `[+0x2c]`, 13 returns a pointer to `this+0x30` (the workforce struct), 14 mayor rating `int8 [+0x34]`, 15 difficulty `[+0x38]`, 16 `GetTaxRate(type)` (float switch over the RCI groups). Explains the NaN "workforce" and the odd ratings in the first dump.
- Everything else in the header (order and argument counts from slot 17 on) matches the binary.

Fix: `src/RegionalCity641.h` calls those slots through the raw vtable with the real signatures; the bridge uses it. Null45's newest header (sc4-region-census) has the same float signatures, so this is worth reporting upstream. `RegionSyncProbe` (getters only) checks the slot 9..16 reading against the UI; `RegionSyncSetterTest` now uses the corrected setters.

```
SetterTest: own SetProduced(2, +1) ...
  readback = 0.00
SetterTest: own SetBudget(+1) ...
  readback = 0
```

Also seen: `GetWorkforcePercentage` returns NaN for every tile, established or not. The bridge now stores 0 for non-finite floats. `GetCityLocations` returned 49 tiles for a 16x16 region with mixed tile sizes; large tiles appear once at their origin.

## Q7. Does the month message fire while paused, and does the game's own save trigger `PostCityShutdown`?

Affects cadence and the "publish on shutdown" path. Low risk either way, but worth confirming from the log.

**Observed (partial, 2026-10-17).** The month message fires once per sim month while running (ten publishes for 2055-01 through 2055-10). Paused behaviour and the save path were not exercised. Side note: `PostRegionInit` logged "Region loaded" twice on one region open; harmless.

## Q8. Where does the game load the DLL from, and where does the store end up?

**Observed (2026-10-17).** The DLL loaded from `Documents\SimCity 4\Plugins`, not the install's Plugins folder, so both locations work. That breaks the "store is shared by construction" assumption in DESIGN.md: `Setup-MultiInstance.ps1` copies the Documents Plugins folder into each instance's private user dir, so each instance would get its own DLL and its own `RegionSync\<region>` store. Fix applied: the script now junctions Plugins into each instance (the per-instance copy also failed on the 260-char path limit inside NAM), so both instances load the same DLL and share its store, and the plugin writes its log into the `-UserDir` folder so the two logs do not collide.

```
Region loaded: 'New York'. Store directory: C:\Users\player\Documents\SimCity 4\Plugins\RegionSync\New York
```
