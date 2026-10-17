# Architecture

DESIGN.md says why. This file says how: what runs where, in what order, and what every file is for. Read both before touching code.

## 1. The host: how SimCity 4 loads a plugin

SimCity 4 (version 1.1.641 only) is a 32-bit Windows process built on Maxis's GZCOM framework, a COM-like object system. At startup the game scans `<install>\Plugins` for DLLs, calls each one's exported `RZGetCOMDllDirector()`, and registers the returned director with the framework. From then on the plugin is just another object inside the game's process with no special privileges and no sandbox: it can call any game interface it can get a pointer to, and it crashes the game if it gets anything wrong.

Three things the framework gives us:

- **Lifecycle hooks.** `cIGZFrameWorkHooks` delivers `PreAppInit`, `PostAppInit`, `PreAppShutdown` and friends. We do all our subscription in `PostAppInit`, when the app services exist.
- **The message server.** `cIGZMessageServer2` broadcasts typed messages (a 32-bit ID plus a few payload words). The game emits messages for city load, city shutdown, region load, sim time passing, cheats typed, and hundreds of other events. A plugin subscribes with `AddNotification(this, messageID)` and receives them in `DoMessage`.
- **Service pointers.** `GZServPtrs.h` wraps `QueryInterface` on well-known services: `cISC4AppPtr` gives the app object, from which everything else hangs (`GetRegion()`, `GetRegionalCity()`, `GetCity()`, `GetCheatCodeManager()`).

None of the `cISC4*` interfaces are documented by Maxis. They are reconstructed vtables in `vendor/gzcom-dll`, correct as far as the community has tested them. Method order is the contract. A header that declares a method we have never called is a hypothesis, not a fact, until the log shows it returning sane values.

## 2. The game objects we touch

```
cISC4App
 ├─ GetRegion()         -> cISC4Region          one per loaded region
 │                          └─ GetCity(x,z)     -> slot holding cISC4RegionalCity, one per tile
 ├─ GetRegionalCity()   -> cISC4RegionalCity    the record for the tile currently open
 └─ GetCity()           -> cISC4City            the live, simulated city (only one)
      ├─ GetSimulator()           -> cISC4Simulator         sim clock, date, pause
      └─ GetNeighborsSimulator()  -> cISC4NeighborsSimulator connections and deals
                                      ├─ cISC4NeighborConnection*  road/rail/pipe/etc edges
                                      └─ cISC4NeighborDeal*        power/water/garbage deals
```

The important distinction is **city** versus **regional city**. `cISC4City` is the full simulation and exists only for the tile you have open. `cISC4RegionalCity` is the lightweight record the region keeps for every tile, loaded from the tile's `.sc4` save file and refreshed from the live city when you save (`UpdateCityCache(cISC4City*)`). Every cross-tile effect in the game reads regional cities, never cities. That is the seam this plugin sits on: it keeps the regional records in one process current with the live cities in other processes.

## 3. Process topology

```
   Instance A (SimCity 4.exe, -UserDir InstanceA)      Instance B (-UserDir InstanceB)
   ┌───────────────────────────────────────┐            ┌───────────────────────────────────────┐
   │ live city: tile (2,3)                 │            │ live city: tile (3,3)                 │
   │ regional records: all tiles           │            │ regional records: all tiles           │
   │ SC4RegionSync.dll                     │            │ SC4RegionSync.dll                     │
   │   publish own record  ──────────┐     │            │     ┌─────────  publish own record    │
   │   pull others' records ◄──────┐ │     │            │     │ ┌───────► pull others' records  │
   └───────────────────────────────┼─┼─────┘            └─────┼─┼─────────────────────────────┘
                                   │ │                        │ │
                       ┌───────────▼─▼────────────────────────▼─▼───────────┐
                       │ <install>\Plugins\RegionSync\<region dir>\          │
                       │   2_3.snapshot   3_3.snapshot   4_3.snapshot ...    │
                       └─────────────────────────────────────────────────────┘
                                         shared store (filesystem, phase 1)

   Both instances also share  Documents\SimCity 4\Regions\<region>\  via a junction,
   which is where the .sc4 save files live. The plugin never writes those.
```

Each instance is an ordinary copy of the game with its own user directory (preferences, thumbnails, Albums) so the two do not fight over config files, and a junction pointing `Regions` back at the real folder so both see the same region and the same save files. `scripts/Setup-MultiInstance.ps1` builds this layout.

The install's `Plugins` folder is shared by construction, which is why the snapshot store defaults to living under it: zero configuration for the common case.

## 4. Data flow

### Publish (every N sim months, on city load, on city shutdown, or `RegionSyncPublish`)

1. `pOwnRegionalCity->UpdateCityCache(pCity)` copies the live city's totals into this tile's regional record, the same call the game makes on save. (Side effects: open question Q1.)
2. `CaptureRegionalCity(pOwnRegionalCity, pSimulator, snapshot)` reads the record into a `NeighborSnapshot`: position, size, serial, name, established flag, population, jobs, workforce %, mayor rating, budget, income, per-commodity exported/imported/produced/demanded/costPerUnit for 8 slots, per-RCI-group population for 12 groups, plus the writer's sim date and wall-clock time.
3. `WriteSnapshotFile` serializes to `key=value` text, writes to `<x>_<z>.snapshot.tmp`, renames over `<x>_<z>.snapshot`. Rename is atomic on NTFS so the other instance never reads a torn file.

### Pull (every N sim months, on city load, or `RegionSyncPull`)

1. Enumerate `*.snapshot` in the store. Skip our own tile.
2. For each, `FindRegionalCity(pRegion, x, z)` resolves the regional record for that tile in **this** process.
3. `ApplySnapshotToRegionalCity` writes every field that has a declared setter and differs: established, budget, income, commodity stats. It counts changed fields and returns the count. Population and jobs are read-only on the interface (open question Q2), so they are logged but not written.
4. If anything changed, `pNeighborsSim->MarkDealsDirty()` so deal amounts, costs and availability are recomputed from the updated records.

### What the game does with the updated record

Nothing the plugin controls. The demand simulator consults regional totals when computing RCI caps; the neighbors simulator consults the other tile's commodity balance when sizing deals; connections consult the other tile's record for commute capacity. Whether each of those reads the record live or cached a copy at city load is the central unknown, and `docs/OPEN-QUESTIONS.md` Q2 is the experiment that settles it.

## 5. Plugin lifecycle (one instance)

```
game start
  RZGetCOMDllDirector()      static RegionSyncDllDirector constructed; Logger initialized
  OnStart(cIGZCOM*)          version check (641 or disable); load INI; AddHook(this)
  PostAppInit()              grab cheat manager; subscribe to 6 message IDs
region opened
  PostRegionInit             cache cISC4Region*, region dir name, resolve store path
city opened
  PostCityInit               cache cISC4City*; register 6 cheat codes; subscribe to cheat messages
  PostCityInitComplete       cache regional city, neighbors sim, simulator; publish + pull on load
playing
  SimNewMonth (0x66956816)   increment counters; publish / pull / autosave on cadence
  CheatIssued (0x230E27AC)   dispatch RegionSync* commands
city closed
  PostCityShutdown           publish once more; clear city pointers; unregister cheats
region closed
  PreRegionShutdown          clear region pointer
```

Pointers cached at `PostCityInitComplete` are only valid until `PostCityShutdown`. Every operation null-checks them because cheats and month messages can arrive in odd orders during transitions.

## 6. Source map

```
src/
  RegionSyncDllDirector.cpp   The only file that knows about GZCOM messages, cheats, the
                              app object and lifecycle. Owns all game pointers. Implements
                              Publish / Pull / ReloadAdjacent / Save / Dump / Status.
  RegionalCityBridge.{h,cpp}  Capture a cISC4RegionalCity into a NeighborSnapshot; apply a
                              NeighborSnapshot to a cISC4RegionalCity; resolve a tile slot.
                              Knows the RCI group IDs. No messages, no cheats, no I/O.
  NeighborSnapshot.{h,cpp}    The plain struct that crosses between instances, and its
                              key=value reader/writer with atomic rename. No game headers,
                              no Windows headers. Compiles and tests anywhere.
  SyncSettings.{h,cpp}        INI parsing with safe defaults; resolves the store directory.
  SC4RegionSync.ini           Shipped defaults; lives next to the DLL in Plugins.
  version.h / version.rc      Plugin version and the Win32 VERSIONINFO resource (rc is UTF-16 LE).
  resource.h                  Boilerplate for the .rc.
  SC4RegionSync.vcxproj/.sln  Visual Studio 2022 project, v143, Win32 only.

vendor/                       git submodules, never edited here
  gzcom-dll/                  The GZCOM SDK: every cIGZ*/cISC4* interface, SC4List/SC4Vector/
                              SC4String containers that match the game's memory layout,
                              cRZCOMDllDirector base class, cRZBaseString.
  sc4-dll-utilities/          Null45's Logger, FileSystem (DLL-relative paths), IniReader,
                              SC4VersionDetection, SC4NotificationDialog, string helpers.
  wil/                        Microsoft's Windows Implementation Library, used by the utilities.
  xwin/                       (created by scripts/setup-toolchain.sh, gitignored) MSVC CRT and
                              Windows SDK headers/libs for cross-compiling on Linux.

scripts/
  setup-toolchain.sh          Fetch xwin and the MSVC/SDK splat (Linux build prerequisite).
  Setup-MultiInstance.ps1     Per-instance user dirs + Regions junction + launcher (Windows).

docs/
  DESIGN.md                   Why this seam, why file-based store, what stays local.
  ARCHITECTURE.md             This file.
  PHASES.md                   Ordered plan with exit criteria.
  OPEN-QUESTIONS.md           Experiments with an "Observed" slot each. The project's truth table.

Makefile                      Linux cross-compile (clang-cl + lld-link + llvm-rc against xwin).
CLAUDE.md                     Agent onboarding and rules.
```

## 7. Build pipelines

Three ways to get `SC4RegionSync.dll`; they should produce functionally identical output.

| Pipeline | Where | Toolchain | When to use |
|---|---|---|---|
| `make` | Linux | clang-cl 20 + lld-link + llvm-rc against xwin splat | Day-to-day on the Linux dev box. This is how the NAM team ships NAM.dll. |
| Visual Studio 2022 | Windows | MSVC v143 | Debugging with breakpoints attached to the game. |

Non-negotiables for all three: target is i386 (Win32), C++20, `/EHsc`, dynamic CRT (`/MD`), and the exported symbol is `RZGetCOMDllDirector`. The game is 32-bit; a 64-bit DLL will simply not load, with no error message.

## 8. Constraints that shape every decision

- **One thread.** All game callbacks arrive on the main thread. The plugin does no threading. File I/O in the message handlers is cheap enough at monthly cadence; if it ever is not, move the store scan to a worker and hand results back through a queue drained on the next month message.
- **No exceptions across the boundary.** GZCOM is C++ but the game was built with a different compiler era. Catch everything inside `DoMessage`; never let an exception propagate into game code.
- **Memory layout is law.** `SC4List`, `SC4Vector`, `SC4String` exist because the game passes STLport-4.6-era containers by reference. Use them, not `std::` equivalents, wherever a game method signature names them.
- **Ref counting is the game's.** `cISC4Region::GetCity` hands back the region's own slot; do not `AddRef`/`Release`. Objects we create ourselves (none so far) would follow normal `cRZAutoRefCount` rules.
- **Version-locked.** Everything is tested against 1.1.641. `OnStart` disables the plugin on any other version rather than guessing.
- **Logs are the instrument.** With no debugger attached to a live two-instance session, `SC4RegionSync.log` is how experiments are read. Log at Info for anything an experiment in OPEN-QUESTIONS.md depends on.
