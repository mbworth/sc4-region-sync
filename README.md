# sc4-region-sync

A DLL plugin for SimCity 4 that lets you play several cities in one region at the same time, each in its own game instance, with the region state kept current between them.

The game only ever simulates one tile per process. What it knows about neighboring tiles is a small summary record per tile: population, jobs, imports and exports, deals. This plugin publishes that record from each running instance and pulls the other instances' records in, so your city's regional demand and neighbor deals track what is actually happening next door instead of what was there the last time you loaded.

Status: scaffold. Builds against the published SDK, nothing verified in-game yet. See `docs/PHASES.md` for the plan and `docs/OPEN-QUESTIONS.md` for the experiments that decide whether the plan survives contact with the engine.

## Requirements

- SimCity 4 Deluxe, version 1.1.641 (the GOG or Steam digital release). Disc versions and the macOS port will not load DLL plugins.
- Either a Linux box with clang 16+, lld and llvm (for `llvm-rc`), or Visual Studio 2022 with the C++ desktop workload. The game is 32-bit, so the only target is Win32.
- Git with submodules.

## Build

On Linux (clang-cl + lld-link against the MSVC headers and libs fetched by xwin, the same way the NAM team builds NAM.dll):

```
git clone --recurse-submodules <this repo>
cd sc4-region-sync
scripts/setup-toolchain.sh --apt     # once; ~1 GB download from Microsoft
make                                 # -> build/SC4RegionSync.dll
```

On Windows with Visual Studio 2022:

```
msbuild src\SC4RegionSync.sln /p:Configuration=Release /p:Platform=x86 /p:SC4PluginsDir="C:\Path\To\SimCity 4 Deluxe Edition\Plugins"
```


`SC4PluginsDir` is optional. When set, the DLL and `SC4RegionSync.ini` are copied into the game's Plugins folder after every build. You can also set the `SC4_PLUGINS_DIR` environment variable instead. Note that this is the Plugins folder inside the game install, not the one under Documents.

Or open `src\SC4RegionSync.sln` in Visual Studio, pick Release | x86, and build. To debug, set the project's debugging command to `SimCity 4.exe` and the working directory to its folder; the game loads the DLL from Plugins at startup.

## Install

Copy `SC4RegionSync.dll` and `SC4RegionSync.ini` into `<game install>\Plugins` (or `Documents\SimCity 4\Plugins`; both load). The plugin writes `SC4RegionSync.log` next to the DLL. With the multi-instance script both locations are shared between instances, and each instance's log goes into its own user dir.

## Running several instances

```
powershell -ExecutionPolicy Bypass -File scripts\Setup-MultiInstance.ps1 -GameExe "C:\...\Apps\SimCity 4.exe" -Setup
powershell -ExecutionPolicy Bypass -File scripts\Setup-MultiInstance.ps1 -GameExe "C:\...\Apps\SimCity 4.exe" -Launch -ExtraArgs "-w -CustomResolution:enabled -r1600x900x32"
```

Setup makes a private user dir per instance under `Documents\SimCity 4 Instances\InstanceN` with its own Albums and preferences, and junctions back to your real `Regions` and `Plugins` folders. Launch starts each instance with `-UserDir` pointing at its own dir. Open a different tile in each instance; if two instances open the same tile the second logs `Conflict:` and stops publishing and pulling until that city is closed (the first opener keeps the tile, the game's own save still works). The game normally refuses a second copy: the plugin closes the game's single-instance mutex at startup (`ReleaseInstanceMutexOnStart=1`), and on Steam, Setup also drops a `steam_appid.txt` next to the exe so the launch does not go back through Steam.

**Steam:** that file lives under Program Files, so run Setup once from an elevated PowerShell, or create `steam_appid.txt` containing `24780` next to `SimCity 4.exe` yourself. Steam must be running when you launch. The launcher never writes there; it checks for the file and tells you if it is missing.

**GOG and disc:** untested. Everything so far has been exercised on the Steam copy only. Those builds do not relaunch through a client, so in principle the mutex release alone should allow a second instance and no `steam_appid.txt` is needed. If you try it, report what happens.

### Launcher

`build/SC4RegionSyncLauncher.exe` (`make launcher`, deployed to `C:\Users\<you>\SC4RegionSyncLauncher\`) lists every running instance with its state and open tile, raises an instance's window on click, and starts the next free `InstanceN` with **Launch**. Run `Setup-MultiInstance.ps1 -Setup -Instances 16` once to create the user dirs it picks from. The status bar estimates how many more instances fit (free RAM and commit against the largest running instance, physical cores minus one per instance); red refuses a launch, amber only warns. The per-instance floor defaults to 1.5 GB; a large city measured higher should raise `PerInstanceRamMb`. **Grid** on the toolbar shows every instance as a live thumbnail (composited by Windows, no cost to the games); click a cell to raise it. **Settings** on the toolbar sets the game exe, resolution, windowed, skip intro, single CPU, renderer, extra arguments and the resource limits; it writes `launcher.ini` next to the exe. A game process with no plugin status (a hung exit, or a copy running without the plugin) is listed as an orphan after 90 s and can be ended with **End Task**; rows with a status file never get that button. Each instance writes `Plugins\RegionSync\_instances\<pid>-<start>.status` for the launcher; instances started any other way show up too.

## Cheat console commands

Open the cheat box with Ctrl+X.

| Command | What it does |
|---|---|
| `RegionSyncStatus` | Dialog with instance id, tile, counts, store path. |
| `RegionSyncDump` | Writes the whole region table, this tile's connections, and its deals to the log. The main research tool. |
| `RegionSyncPublish` | Refresh own regional record from the live city and write the snapshot. |
| `RegionSyncPull` | Read every other tile's snapshot and apply it. |
| `RegionSyncReload` | Experimental: ask the region to reload adjacent tiles' records from their save files. |
| `RegionSyncSave` | Fast-save the current city. |
| `RegionSyncMirror` | Create other instances' published connections and deals in this city now, ignoring `SyncConnections`/`SyncDeals` (otherwise this happens on the sim day after each pull), and lay the 3-cell edge network piece under each mirrored connection (a 2 by 3 block for avenue and highway lane pairs; demolishing what is on those cells, as the game does at load), ignoring `LayEdgeNetwork`. Ferry connections are logged, not mirrored. Also removes connections and deals a live neighbor no longer lists (no demolition), ignoring `SyncRemovals`. |
| `RegionSyncOffer` | Diagnostic: logs each neighbor/deal-type check behind a deal offer, then what `ProposeDeals` (not added to the sim) returns. |
| `RegionSyncHoldLock` | Diagnostic toggle: take the region save lock, release it on the next use or at city shutdown. The other instance's saves wait (up to `SaveLockTimeoutMs`) meanwhile; after one times out, the next save tries once without waiting. |

## Settings

See `src\SC4RegionSync.ini`. Everything has a default; with no INI the plugin runs with monthly publish and pull, autosave off, and Info logging.

- `SaveLock` (default 1): every save holds a region-wide named mutex, so two instances never write each other's `.sc4` files at the same time. Both instances need it on; the log shows `saveLock=`, `hook=` and `peer=`.
- `SaveLockTimeoutMs` (default 15000): longest a save waits for the other instance before it saves unlocked (logged). 0 = never wait.
- `PullOnStoreChange` (default 1, Q17): pull on the sim day after a peer's snapshot lands. Monthly pull stays as fallback; `PullEveryMonths=0` does not disable this.
- `ReloadOnPeerSave` (default 0, experiment Q17): in the region view, reload a tile's record when another instance saves it.

## Layout

- `src/` the plugin. `RegionSyncDllDirector.cpp` is the entry point and the only file that knows about messages and cheats. `RegionalCityBridge` moves data between the game's regional records and `NeighborSnapshot`, which is the plain struct that crosses between instances and has no game dependency.
- `vendor/` git submodules: `gzcom-dll` (the SC4 GZCOM SDK), `sc4-dll-utilities` (Null45's logging, INI and version-detection helpers), `wil`.
- `docs/` design, architecture, phases, open questions.
- `scripts/` multi-instance setup.

## Credits

Built on [gzcom-dll](https://github.com/nsgomez/gzcom-dll) by Nicholas Gomez and the interface work by Nicholas Hayes (Null45), whose [sc4-dll-utilities](https://github.com/0xC0000054/sc4-dll-utilities) and published plugins set the conventions this project follows.

## License

Plugin code: MIT. Vendored libraries keep their own licenses (gzcom-dll and sc4-dll-utilities are LGPL 2.1 or later; wil is MIT).
