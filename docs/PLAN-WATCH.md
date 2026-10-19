# Plan: change notification and region-view refresh (Q17)

Reviewed 2026-10-19 in one round (game safety, threading, simplicity). The round removed the watcher threads and the name parsing (signal-only handles polled from the game thread), moved the store-triggered pull onto the sim-day tick, made the region reload a try-lock, and split the work into two slices with the region refresh shipped off by default.

Two items that share one mechanism: a `FindFirstChangeNotificationW` handle per watched directory, polled with a zero-timeout wait from the game thread. No thread, no buffer, no names. A signal means "rescan", and the rescan is a modification-time pass over a dozen files.

## Slice A: pull on store change (default on)

**Today** a peer's snapshot lands on the next sim-month tick. **After** it lands on the next sim-day tick (seconds at normal speed) following the write.

- `PostRegionInit`: `std::filesystem::create_directories(storeDirectory)` (it is otherwise created on first publish), then open the handle: `FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)`. Idempotent: if already open for the same path do nothing; if open for another path close first. Log `StoreWatch: opened <dir>` or the `GetLastError` at Warning (then the monthly pull is the only cadence). `PostRegionInit` runs twice per city load (the message, then the late call from `PostCityInitComplete`), so the idempotence matters.
- `PreRegionShutdown` and `PreAppShutdown`: `FindCloseChangeNotification`, handle set to invalid. In `PreAppShutdown` this happens before `savelock::Close()`.
- `OnNewDay`, before the existing publish check: if `settings.pullOnStoreChange`, the handle is valid, `pCity && pOwnRegionalCity`, and `WaitForSingleObject(h, 0) == WAIT_OBJECT_0`: call `FindNextChangeNotification` (on failure log once at Warning and close the handle), then scan `storeDirectory` for `*.snapshot` other than our own tile's file and compare `last_write_time` with `storeSeen` (a `std::map<std::string, file_time_type>` the director keeps). If any entry is new or changed, `Pull("store change")`. `storeSeen` is refreshed at the end of every `Pull` whatever its reason, so a monthly pull also resets it.
- Nothing else changes. `Pull` already takes `InPullScope` and checks `ConflictRefuses`. `OnNewMonth` is untouched; `PullEveryMonths` stays as the fallback cadence.
- Our own publish signals the handle too. The scan sees no peer change and nothing happens; one Debug line `StoreWatch: signal, no peer change`.
- Why the day tick and not idle: `Pull` writes records, feeds demand deltas and marks deals dirty. Every run so far has been from a sim tick or a cheat; idle can fire inside a modal dialog or a nested message pump. The day tick is the context the mirror already uses.

Setting: `PullOnStoreChange=1`. INI comment says the monthly pull stays and `PullEveryMonths=0` does not disable this path.

## Slice B: region-view refresh (default off, experiment)

**Today** an instance in the region view shows neighbors as loaded. **After** a peer's save reloads that tile's regional record while the region view is up. Whether the picture redraws is the experiment.

Game plumbing this slice adds, all new to this project and logged at Info on first use:

- `cISC4App::GetRegionsDirectory()`: called in `PostRegionInit`; the regions folder is that plus `\` plus the region directory name. Log the path and whether `region.ini` exists there. Nobody in Null45's repos calls it; treat as an experiment.
- An idle callback: `src/IdleService.{h,cpp}`, a `cRZBaseSystemService` subclass (vendored, already in `gzcom-dll/src`), service ID `0x51CDBF59` (random, listed in the director's ID block, checked against our IDs and Null45's `0x3fa8cbb9` auto-save and his rich-presence service). `OnIdle` calls a `std::function<void()>` inside `try/catch(...)`, with the catch logging once per session. Registered in `PostAppInit` only when `ReloadOnPeerSave=1`: `AddSystemService` then `AddToOnIdle` (Null45's sc4-auto-save `cGZAutoSaveService` and sc4-discord-rich-presence do exactly this). Removed in `PreAppShutdown` in reverse, after the callback is cleared. Log `IdleService: added=%d onIdle=%d`. The service object is a director member; the director is static and outlives the framework's last Release, same as Null45's pattern.
- `cISC4Region::ReloadCity` in the region view. Q3 exercised it only with a city open. The region view may hold record pointers (hover, info panel). So the cheat comes first: `RegionSyncReload` is also registered in `PostRegionInit` (cheat registration in the region view is itself unobserved; log whether the cheat fires) and in the region view it reloads every established tile. The human then hovers, selects and opens a reloaded tile. Only after that is observed does the timer path get enabled.

The timer path (`OnIdle`, when `ReloadOnPeerSave=1`):

1. Skip if `savehook641::InSave()` (new accessor over `saveDepth`), or `!regionViewReady`, or `cISC4AppPtr->GetRegion() != pRegion`.
2. If the region handle is signalled: `FindNextChangeNotification`, stamp `regionSignalTick = GetTickCount64()`, set `regionPending`.
3. If `regionPending` and `now - regionSignalTick >= kWatchQuietMs` (constexpr 750): probe every established tile (`GetCityLocations`, `FindRegionalCity`, `GetEstablished`, `GetCitySaveFilePath`): size and `last_write_time`. A tile is due when its probe differs from `regionSeen` and equals the previous idle's probe (stable across two idles) and size is non-zero. Tiles with an empty save path are logged once at Info and skipped. If nothing is due, clear `regionPending`.
4. `savelock::TryAcquire("watch")`: new function, `WaitForSingleObject(mutex, 0)`, never touches the timeout back-off. Busy: log once per burst at Info and try again next idle. Held: `ReloadTile(x, z)` for each due tile, release, update `regionSeen`, clear `regionPending`.

`regionViewReady` is set only by the `PostRegionInit` message, not by the late call, and cleared in `PreRegionShutdown`, `PostCityInit` and `PostCityShutdown`. `PostCityShutdown` logs at Info so the experiment records whether a region-init message follows a city exit; if none does, the timer path never fires and the log says so.

`ReloadTile(x, z)` is `ReloadAdjacent`'s loop body pulled into a helper: `GetCity`, `ReloadCity`, log `Reload (x,z): ok=%d popBefore=%d popAfter=%d recordPtrChanged=%d name='%s'->'%s'`. `ok` is not a success flag (Q3); `recordPtrChanged` is. `ReloadAdjacent` (the in-city cheat) calls it and keeps its `MarkDealsDirty`.

Setting: `ReloadOnPeerSave=0` until Q17 step 4 is observed. Everything in this slice is inert at 0: no service, no handle, no `GetRegionsDirectory` call. The region-view `RegionSyncReload` cheat is registered regardless, because it is the experiment.

## Must not break

- Never game code off the game thread: there is no other thread.
- Save lock (Q4): the region reload only ever try-locks, so it cannot block the UI and cannot flip the lock into its post-timeout "go ahead unlocked" mode. Writers that bypass the lock (peer with `SaveLock=0`, MoveCity) are covered by the two-idle stability probe and the non-zero size check.
- No reload while a city is open or loading: `regionViewReady` is false from `PostCityInit` until the next region-init message.
- `ReloadCity` replaces record objects: the mirror already re-resolves; in the region view the mirror is idle.
- No exception reaches game code: `OnIdle` and the new `OnNewDay` branch are inside try/catch.
- `make test` untouched.

## Experiment (Q17)

Two instances on New York, instance 1 BumperVille (4,6), instance 2 TuggersVille (6,6).

1. Both logs: `StoreWatch: opened ...`. With `ReloadOnPeerSave=1` also `RegionsDirectory: '<path>' regionIni=1` and `IdleService: added=1 onIdle=1`.
2. Slice A: wait for a month tick in 2. Within a sim day, 1 logs `Pull (store change): ...` before its own month tick. `Pull (month tick)` still appears monthly. Instance 1's own month publish produces `StoreWatch: signal, no peer change` at Debug and no pull.
3. Cheat in region view: instance 1 exits to the region view. Log shows the `PostCityShutdown` line and whether `PostRegionInit` (message) followed. Type `RegionSyncReload`. Does the cheat fire (`Reload (x,z) ...` lines)? Hover, select, and open a reloaded tile. Game stable?
4. Timer path (`ReloadOnPeerSave=1`): 1 in region view, 2 saves. Within about a second of 2's `SaveLock: released`, 1 logs `RegionWatch: N tile(s) due` and `Reload (6,6): ... recordPtrChanged=1`. Did TuggersVille's thumbnail and hover population change without leaving the region view? Yes or no goes in the Observed slot.
5. Danger window: 2 saves, and the human clicks Play on a tile in 1 inside the next second. Expect `regionViewReady` cleared by `PostCityInit` before any reload, and the city loads.
6. 1 exits a city to the region view while 2 is saving (`RegionSyncHoldLock` in 2 to widen it). Expect `RegionWatch: busy` lines, then the reload once 2 releases. 2's save succeeds.
7. Negative control: 2's own save never reloads anything in 2.
8. Ten minutes at cheetah, both in city: no Warning lines, store-change pulls arrive per peer month tick.

If step 4 reloads the record but the picture does not change, the region view caches the thumbnail. That is a separate slice (Ghidra: the region view's tile draw path). Not in this plan.

## Docs and build to amend

- `docs/OPEN-QUESTIONS.md`: `## Q17` after Q16 with the steps above and an empty Observed slot.
- `docs/PHASES.md`: tick the Phase 2 change-notification box when step 2 is observed; the Phase 3 region-view box when step 4 is.
- `docs/ARCHITECTURE.md`: lifecycle rows (handles opened in `PostRegionInit`, closed in `PreRegionShutdown`/`PreAppShutdown`, idle service add/remove, the `OnNewDay` store check, `OnIdle` row); source map entries for `IdleService.{h,cpp}`.
- `README.md`: the two settings; `RegionSyncReload` noted as region-view capable once observed.
- `src/SC4RegionSync.ini`, `src/SyncSettings.{h,cpp}`, the `Initialized.` log line: `pullOnStoreChange=%d reloadOnPeerSave=%d`.
- `Makefile` `SRCS` and `src/SC4RegionSync.vcxproj`: `IdleService.cpp`, and `vendor/gzcom-dll/gzcom-dll/src/cRZBaseSystemService.cpp` if not already linked.
- `CLAUDE.md` status line.

## Not in this plan

- Reloading a peer's record while our own city is open (the mirror and pulls cover it).
- Dropping the monthly cadence.
- Network shares: `FindFirstChangeNotification` works over SMB but delivery is not guaranteed; the monthly pull is the safety net.
