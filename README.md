# sc4-region-sync

A DLL plugin for SimCity 4 that lets you play several cities in one region at the same time, each in its own game instance, with the region state kept current between them.

The game only ever simulates one tile per process. What it knows about neighboring tiles is a small summary record per tile: population, jobs, imports and exports, deals. This plugin publishes that record from each running instance and pulls the other instances' records in, so your city's regional demand and neighbor deals track what is actually happening next door instead of what was there the last time you loaded.

Status: scaffold. Builds against the published SDK, nothing verified in-game yet. See `docs/PHASES.md` for the plan and `docs/OPEN-QUESTIONS.md` for the experiments that decide whether the plan survives contact with the engine.

## Requirements

- SimCity 4 Deluxe, version 1.1.641 (the GOG or Steam digital release). Disc versions and the macOS port will not load DLL plugins.
- Visual Studio 2022 with the C++ desktop workload (v143 toolset, Windows 10 SDK). The game is 32-bit, so the only target is Win32.
- Git with submodules.

## Build

```
git clone --recurse-submodules <this repo>
cd sc4-region-sync
msbuild src\SC4RegionSync.sln /p:Configuration=Release /p:Platform=Win32 /p:SC4PluginsDir="C:\Path\To\SimCity 4 Deluxe Edition\Plugins"
```

`SC4PluginsDir` is optional. When set, the DLL and `SC4RegionSync.ini` are copied into the game's Plugins folder after every build. You can also set the `SC4_PLUGINS_DIR` environment variable instead. Note that this is the Plugins folder inside the game install, not the one under Documents.

Or open `src\SC4RegionSync.sln` in Visual Studio, pick Release | x86, and build. To debug, set the project's debugging command to `SimCity 4.exe` and the working directory to its folder; the game loads the DLL from Plugins at startup.

## Install

Copy `SC4RegionSync.dll` and `SC4RegionSync.ini` into `<game install>\Plugins`. The plugin writes `SC4RegionSync.log` next to the DLL.

## Running two instances

```
powershell -ExecutionPolicy Bypass -File scripts\Setup-MultiInstance.ps1 -GameExe "C:\...\Apps\SimCity 4.exe" -Setup
powershell -ExecutionPolicy Bypass -File scripts\Setup-MultiInstance.ps1 -GameExe "C:\...\Apps\SimCity 4.exe" -Launch -ExtraArgs "-w -CustomResolution:enabled -r1600x900x32"
```

Setup makes a private user dir per instance under `Documents\SimCity 4 Instances\InstanceN` with its own Plugins and Albums and a junction back to your real `Regions` folder. Launch starts each instance with `-UserDir` pointing at its own dir. Open a different tile in each instance.

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

## Settings

See `src\SC4RegionSync.ini`. Everything has a default; with no INI the plugin runs with monthly publish and pull, autosave off, and Info logging.

## Layout

- `src/` the plugin. `RegionSyncDllDirector.cpp` is the entry point and the only file that knows about messages and cheats. `RegionalCityBridge` moves data between the game's regional records and `NeighborSnapshot`, which is the plain struct that crosses between instances and has no game dependency.
- `vendor/` git submodules: `gzcom-dll` (the SC4 GZCOM SDK), `sc4-dll-utilities` (Null45's logging, INI and version-detection helpers), `wil`.
- `docs/` design, phases, open questions.
- `scripts/` multi-instance setup.

## Credits

Built on [gzcom-dll](https://github.com/nsgomez/gzcom-dll) by Nicholas Gomez and the interface work by Nicholas Hayes (Null45), whose [sc4-dll-utilities](https://github.com/0xC0000054/sc4-dll-utilities) and published plugins set the conventions this project follows.

## License

Plugin code: MIT. Vendored libraries keep their own licenses (gzcom-dll and sc4-dll-utilities are LGPL 2.1 or later; wil is MIT).
