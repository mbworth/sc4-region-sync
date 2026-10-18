# Plan: instance status file and launcher (Phase 3 coordinator, slice 1)

Goal: start and switch between many instances quickly, see which tile each has open, and be told before launching one more than the machine can carry. Grid view (DWM thumbnails) is slice 2 on the same window list.

Reviewed 2026-10-18 (design and operations lenses). v1's snapshot-based tile guard, `PostCityInit` placement, in-launcher junction creation, Close button and tab control were dropped on their findings; v2 below.

Box today: 8 logical cores, 32 GB, RTX 3060 Ti. SC4 is a 32-bit, mostly single-threaded, large-address-aware process (checked in the exe header).

## Part A: plugin

### A1. Status file

`<store base>\_instances\<pid>-<start>.status`, where store base is `settings.ResolveStoreDirectory("")` (so `StoreDirectory` is honoured and `_instances` cannot collide with a region directory name). Same key=value text as the snapshots, written by the director only. The `<start>` suffix is the process creation FILETIME: a reused pid never names an old file, so the stale-file reaper can never delete a live instance's file.

```
; sc4-region-sync instance status
schemaVersion=1
host=GAMEBOX
pid=29420
start=134368212816162474
userDir=C:\Users\player\Documents\SimCity 4 Instances\Instance1
state=city
region=New York
regionX=4
regionZ=6
cityName=BumperVille
conflict=0
writtenUnixTime=1792345678
```

(`LoadKeyValues` does not strip inline comments, so values carry none.) The file is UTF-8; region and city name are converted from the game's strings when written. `state` is one of `starting`, `region`, `city`. `userDir` is the `-UserDir` argument normalised: no trailing backslash; empty when the game runs without one. No window handle: the launcher finds the window by pid.

| Event | state | Notes |
|---|---|---|
| `PostAppInit`, after `Unlock("startup")` | starting | host, pid, start, userDir. Written last so "status present" means the next exe can start. If `PostAppInit` bails out earlier (subscription failure) no file is written; the launcher shows that pid as "starting (no status)". Before writing, delete every `*.status` and `*.status.tmp` in `_instances` whose writer is `Dead` (same host; `Unknown` is kept). |
| `PostRegionInit` (and the late path from `PostCityInitComplete`) | region | + region |
| `PreRegionShutdown` | starting | region cleared (main menu, region switch) |
| `PostCityInitComplete`, after the late `PostRegionInit` fallback and before `Publish("city load")` | city | + regionX, regionZ, cityName, conflict |
| `PostCityShutdown` | region | city fields cleared, conflict cleared |
| `PreAppShutdown` | file deleted | |

Writes use a shared `regionsync::WriteFileAtomic(path, text, error)` pulled out of `WriteSnapshotFile` (temp plus rename). The rename is retried a few times with a short sleep because the launcher may hold the file open; the launcher opens with `FILE_SHARE_READ|WRITE|DELETE` so the retry is the exception. Final failure logs once per event and does nothing else.

Code: new game-free `InstanceStatus.{h,cpp}` (struct, `Write`, `Read`, file name helper) beside `NeighborSnapshot`, reusing `WriterIdentity`, `IsValidWriterHost` and an exported `regionsync::LoadKeyValues`. New director helper `UserDirFromCommandLine()` used by `LogFilePath()` too. `InstanceStatus.cpp` joins the `test` target with a round-trip test.

### A2. Same-tile guard

Per-tile named mutex `Local\SC4RegionSync.Tile.<region dir>.<x>_<z>`, same pattern and session scope as `SaveLock`. Created in `PostCityInitComplete` before the status write; `ERROR_ALREADY_EXISTS` means another live instance has this city open: **conflict**. Closed in `PostCityShutdown` and `PreAppShutdown`; the kernel frees it on crash or Exit, so there is no stale state and the check is atomic (the snapshot is not: `PublishOnLoad=0`, simultaneous loads and leftover snapshots all misfire).

A conflicting instance closes its own mutex handle at once (a dark duplicate must not keep the tile alive after the holder leaves) and keeps only the name for the "holder went away" probe (`OpenMutexW`). In conflict the plugin logs once at Error (`Conflict: tile (x,z) 'Name' is open in pid N since <cityLoadedAt from the snapshot, if any>`), writes `conflict=1`, and the flag is tested **inside** `Publish`, `Pull`, `MirrorSnapshot`, the plugin autosave and the `PostCityShutdown` snapshot remove or publish, so no caller (month, day, dirty publish, mirrorPending, cheats) gets around it. Each refused call logs at Debug. Player saves go through the lock as always; the plugin's own autosave is skipped because a save of the duplicate city would overwrite the first opener's `.sc4`. The guard clears on `PostCityShutdown`. When the holder goes away while we are still in conflict we stay dark (our copy is stale) and log that once. No dialog in the plugin; the launcher shows the warning.

Nothing can cancel a city load from a plugin, and the player can open any tile from any region view; refusing to write is what prevents two writers for one tile.

Settings: none new. The status file and the guard are unconditional.

## Part B: launcher (`launcher/`, Win32 C++, same toolchain)

Plain Win32, x86, built by the existing clang-cl plus xwin pipeline as `make launcher` -> `build/SC4RegionSyncLauncher.exe`. Own flag set (no `/LD`, `/MT`, `/SUBSYSTEM:WINDOWS`, `wWinMain`, libs `user32 gdi32 comctl32 shell32 psapi dwmapi`), `launcher.rc` in UTF-8 with the manifest as `RT_MANIFEST` (comctl32 v6, `asInvoker`, `PerMonitorV2`), `InitCommonControlsEx`. `make deploy` copies it to `C:\Users\player\SC4RegionSyncLauncher\`, never into Plugins.

One window: a report-style ListView (columns: instance, state, tile, pid), a toolbar with **Launch** and **Refresh**, a status bar with the resource line. No Close button in this slice: `WM_CLOSE` on the game is untested (does it prompt to save, drop progress, fire `PreAppShutdown`, land inside a held save lock) and the in-game Exit works. Q14 step 5 tests it before slice 2 adds it.

Settings `launcher.ini` next to the exe, created with defaults on first run: `GameExe`, `UserDirRoot` (`Documents\SimCity 4 Instances`), `StatusDirectory` (`Documents\SimCity 4\Plugins\RegionSync\_instances`; set it by hand when the plugin INI sets `StoreDirectory`), `ExtraArgs` (`-w -CustomResolution:enabled -r1600x900x32 -intro:off`), `PerInstanceRamMb` (3500), `ReserveCores` (1), `ReserveRamMb` (4096), `MaxInstances` (16), `WindowClass` (empty = any, filled in once Q14 records the game's class). Refresh re-reads the file.

### Discovery (every second)

Read `_instances\*.status` with share-delete opens; a failed read keeps the row from the previous tick. Writer liveness by pid and creation time with `PROCESS_QUERY_LIMITED_INFORMATION` only; `Unknown` (access denied) counts as alive; only `Dead` files are deleted. Main window per pid: visible, unowned, the game's window class (read once with Spy++ and recorded in Q14), cached and rechecked with `IsWindow` each tick; "starting (no window)" until found. Rows are labelled from the status file (`Instance1`, from the userDir's last component, or the pid when it is empty), so task-started and hand-started instances appear too. A conflict row shows `CONFLICT` and, when another live non-conflicting status names the same region and tile, that holder's pid in the tile column.

### Launch

Preconditions, each with a message when it fails: `GameExe` exists; `steam_appid.txt` exists next to a `steamapps` exe (the launcher never writes under Program Files: a 32-bit write there gets virtualised and the game never sees it, Q5 created the file by hand); `steam.exe` running for a Steam exe; `ExtraArgs` contains `-w` (a fullscreen DirectDraw instance switches display modes when raised); no launch in flight; the resource line is not red; `MaxInstances` not reached; a free `InstanceN`.

A free `InstanceN` is an existing dir under `UserDirRoot` (created once by `scripts/Setup-MultiInstance.ps1 -Setup -Instances 16`, which stays the only place that builds the layout) whose normalised path (case-insensitive, no trailing backslash) matches no live status file and no pending launch. Pending launches are an in-memory map user dir -> pid, kept until that pid's status file appears or the pid exits; after 60 s a message is shown and Launch is re-enabled, but the dir stays reserved until the pid exits. A pid that exits with no status file is reported once (Q5's silent quit). Paths are compared after full-path and long-name normalisation, case-insensitive, no trailing backslash. While one is pending, Launch is disabled: the game closes its single-instance mutex only in `PostAppInit`, so a second exe started before that finds the mutex and quits silently (Q5).

Command line built by hand, no quoting helper (Q5): `"<exe>" -UserDir:"<dir>\" <ExtraArgs>`, `CreateProcessW` with the exe's folder as working directory.

### Raise

Click, double-click or Enter on a row: `ShowWindowAsync(SW_RESTORE)` when `IsIconic`, then `SetForegroundWindow` (the launcher is the foreground process at that moment, so no `AllowSetForegroundWindow`). Nothing is hidden or reparented; the other instances keep running and simulating unfocused (Q13 logs). The grid (slice 2) adds DWM thumbnails of the same windows, view only, click to raise.

### Resource line

Sampled every 5 s (the 1 s tick only reads files and windows):

- **Need per instance** = max(`PerInstanceRamMb`, largest `WorkingSetSize` seen among running instances this session). An instance at the region view is far smaller than one with a big city, hence the floor.
- **RAM room** = `(ullAvailPhys - ReserveRamMb) / need`; **commit room** = `(ullAvailPageFile - ReserveRamMb) / max(PerInstanceRamMb, largest PrivateUsage)`. Memory room is the smaller.
- **CPU room** = physical cores (`GetLogicalProcessorInformationEx`) minus `ReserveCores` minus one per running instance, whatever its current load (paused instances unpause together). The 5 s `GetSystemTimes` idle figure is shown, and under one core's worth turns the line amber.
- **Room** = min(memory room, CPU room, `MaxInstances` - running). Text: `N running, ~M more at full speed (estimate)`; tooltip names the binding resource and the three raw numbers.
- Colours: red = memory room 0 (launch refused, numbers in a message box); amber = CPU room 0 or idle under one core (launch allowed, the next one slows everyone); green otherwise. No prompt on amber.
- GPU and VRAM not measured; Q14 step 3 records the per-instance VRAM from Task Manager so "not the limit" rests on a number.

The 16 ceiling comes from the slice 2 grid, not from the machine. On this box memory binds near 7 to 8 instances and CPU near 7; the status line will say which.

### Reads and writes

The launcher reads status files, the plugin INI is not read; it writes `launcher.ini`, deletes only `Dead` status files. It never touches snapshots, `.sc4` files, the Regions folder, the plugin INI or anything under Program Files.

## What must not break

- Instances started by the scheduled tasks or by hand keep working and show up in the launcher.
- No new game calls in the plugin (the messages used are subscribed today). `_instances` unwritable: log once, run as before.
- Publish, pull, mirror and autosave are unchanged for a non-conflicting city. A conflict never blocks a player save.
- `NeighborSnapshot.{h,cpp}` and `InstanceStatus.{h,cpp}` stay game-free; `make test` covers the status round trip.
- `_instances` sits beside the per-region folders; nothing that enumerates `<region>\*.snapshot` sees it.
- `make deploy` still refuses while the game runs.

## Experiment (Q14)

1. Launch three instances from the launcher one after another (Launch greys out while one is pending): rows go starting -> region -> city as each is opened, pids match the logs, raise works from each row, including a minimised one. Exit one in game: row gone, status file gone. Kill one: row gone on the next tick, stale file deleted by the launcher; relaunch and check the plugin's startup reaper leaves live files alone.
2. Open the same tile in two instances: the second logs `Conflict:`, its `Publish`/`Pull` lines stop (Debug shows the refusals), the first opener's snapshot keeps its pid; the launcher shows CONFLICT. Exit that city: row back to region, and a different city in that instance publishes again. Close the first opener's city while the second is still in conflict: the second stays dark and logs it once.
3. Resource line against Task Manager with 0, 1 and 3 instances (working set, private bytes, VRAM per instance written down); set `ReserveRamMb` absurdly high to see red and the refused launch; set `MaxInstances=3` to see the cap.
4. Instances started by the scheduled tasks appear in the launcher; the game's window class is recorded here.
5. `WM_CLOSE` probe for slice 2: send it by hand (PowerShell) to an instance with a dirty city and to one inside a `RegionSyncHoldLock`; record whether it prompts, saves, fires `PreAppShutdown` and releases the lock.

## Docs and build to amend

`README.md` (launcher section, status file, conflict rule), `ARCHITECTURE.md` (source map, store layout with `_instances`, lifecycle rows, the tile mutex next to the save mutex), `PHASES.md` (Phase 3 coordinator split into slice 1 and grid), `OPEN-QUESTIONS.md` Q14, `Makefile` (`launcher` target, `test` adds `InstanceStatus.cpp`, deploy copies the exe), `src/SC4RegionSync.vcxproj` (new sources), `CLAUDE.md` status line and source map mention.
