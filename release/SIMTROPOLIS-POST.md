**Title:** SC4RegionSync (DLL): play several cities in one region at the same time

About This File

SC4RegionSync lets you run two or more copies of SimCity 4 on one PC, each with a different city from the same region open, and keeps them in step with each other while you play. Each city sees the others' population, jobs, demand, neighbor connections and deals as they change, instead of as they were the last time you saved and reloaded. A small launcher starts and lists the instances, and can keep one region live across two PCs on the same LAN.

Why use it

- Grow a region as a whole. Build the suburb while the downtown runs next to it, and watch the commuters and demand follow.
- Live neighbors. Regional population and demand in your city update within a sim day of the city next door growing.
- Connections and deals both ways. A road, rail or power connection made in one city appears at the matching edge of the neighbor, with the network piece laid. Neighbor deals follow too, including amount changes and cancellations.
- Safe saves. Every save takes a region-wide lock, so two instances never write the same file at once. Autosave with several instances has been checked.
- Launcher. Lists every running instance with its open tile, shows them as live thumbnails, starts the next one, and estimates how many more will fit in your RAM and CPU before it lets you.
- Two PCs. Host the region on one PC and join from another on the same network. The region is copied over once and mirrored live after that.
- Nothing to configure. Defaults are set for play. One click on Setup prepares the per-instance folders.

Requirements

- SimCity 4 Deluxe Edition, version 1.1.641 (Windows). Steam is what it has been played on. GOG should work and is untested; disc versions and the macOS port do not load DLL plugins.
- Windows 10 or later.
- Microsoft Visual C++ 2015-2022 Redistributable (x86). You need the x86 (32-bit) version even on 64-bit Windows, because SimCity 4 is a 32-bit game.
- RAM for each copy of the game. Plan on 1.5 GB per instance plus whatever a big city needs; the launcher refuses a launch when it thinks you are out.

Installing

1. Install the Visual C++ Redistributable (x86) linked above, if you don't have it yet.
2. Unzip the download and copy SC4RegionSync.dll and SC4RegionSync.ini into the top level of your Plugins folder. That is normally <MyDocs>\SimCity 4\Plugins.
3. Copy SC4RegionSyncLauncher.exe anywhere you like, for example a folder of its own under your user folder. It writes launcher.ini next to itself.
4. Start SC4RegionSyncLauncher.exe, open Settings on the toolbar and set the game exe, then click Setup once. It shows what it will do, then creates <MyDocs>\SimCity 4 Instances\Instance1..N, one per copy of the game, each with its own preferences and Albums and with Regions and Plugins pointing back at your real ones. Nothing is copied and nothing in your SimCity 4 folder is changed.

   Steam only: Setup also offers to create steam_appid.txt (the single line 24780) next to SimCity 4.exe so a second copy does not relaunch through Steam. That folder is under Program Files, so Windows asks for administrator approval; decline and you can create the file by hand instead. Steam must be running when you play.
5. In Settings, set the resolution and windowed mode. Click Launch for each copy you want, and open a different city in each.

Checking that it works

Open the cheat box (Ctrl+X) in any city and type RegionSyncStatus. The dialog shows the instance id, the tile it owns, how many other instances it sees and where the shared store is. Each instance also writes SC4RegionSync.log into its own folder under <MyDocs>\SimCity 4 Instances\InstanceN; the Pull: lines name the tiles it picked up.

Uninstalling

Delete SC4RegionSync.dll and SC4RegionSync.ini from your Plugins folder, the launcher folder, and <MyDocs>\SimCity 4 Instances. Your Regions folder is not touched by the plugin. The launcher replaces region files only on a guest PC during a LAN sync, after archiving the originals to <MyDocs>\SimCity 4\RegionSyncLAN\archive.

Two PCs

Both PCs need this same release. One PC hosts the region, the other joins and receives a copy of it.

- Host: click Host on the toolbar. Allow the Windows firewall prompt on Private networks (a Public network profile blocks the guest and the launcher says so). The status strip shows a 6-digit code.
- Guest: click Join, pick the host from the list or type its address, enter the code, Connect, then choose the region. The copy waits until no SimCity 4 is running on the guest; your own folder of that name is archived first.
- After that both PCs mirror live and the other PC's instances show in the list with its name.
- Guest saves stay on the guest in this release; the strip flags the tile "not sent". Re-sync replaces them with the host's copy. Sending guest saves back is the next piece of work.
- The code is single-use. Once paired, the launchers reconnect by themselves. Pair only with your own PC on a network you trust: traffic is signed, not encrypted, and a paired PC's region files are loaded by your game.
- Ports: TCP 47641 and UDP 47642 (Port under [Lan] in launcher.ini). Do not forward them on your router.

Settings

Settings live in SC4RegionSync.ini, next to the DLL. Everything has a default; these are the ones worth knowing.

- PublishEveryMonths, PullEveryMonths (default: 1) How often, in sim months, a city publishes its own record and reads the others'. A pull also happens on the sim day after another instance saves.
- AutoSaveEveryMonths (default: 0) Fast-save on a cadence. Off unless you want it.
- SyncConnections, SyncDeals, SyncRemovals (default: on) Mirror the other instances' edge connections and deals, and remove ones they no longer have.
- LayEdgeNetwork (default: on) Lay the 3-cell network piece under each mirrored connection, demolishing what is on those cells, as the game does at load.
- SaveLock (default: on) The region-wide save lock. Keep it on with more than one instance.
- SaveLockTimeoutMs (default: 15000) Longest a save waits for another instance before saving unlocked.
- ReleaseInstanceMutexOnStart (default: on) Lets a second copy of the game start. Off means only one instance.
- StoreDirectory (default: empty) Where the shared records go. Empty is Plugins\RegionSync\<region folder>, which works when all instances share one Plugins folder.
- LogLevel (default: Info) Troubleshooting only. Debug writes much more detail to the log.

Cheats

Open the cheat box with Ctrl+X. RegionSyncStatus (the dialog above), RegionSyncPublish and RegionSyncPull (publish or pull now, instead of waiting for the month), RegionSyncMirror (create the other instances' connections and deals now), RegionSyncSave (fast-save), RegionSyncDump (write the whole region table, this tile's connections and its deals to the log).

Reporting bugs

Please report problems on GitHub or in this file's comments. Attach SC4RegionSync.log from every instance that was running, from the session that went wrong. The log is replaced every time the game starts, so copy it before you launch again. For two-PC problems add lan.log from <MyDocs>\SimCity 4\RegionSyncLAN on both PCs. If you can reproduce the problem, set LogLevel=Debug first.

Background

SimCity 4 simulates one city per process, and what a city knows about its neighbors is a small summary record per tile that the game refreshes from the save files when a city loads. That record turned out to be the seam: publish it from each running copy, apply the others' records on a sim-day tick, and feed the change into the demand simulator the way the game does at load. Connections and deals go the same way through the game's own neighbor simulator. The regional view still redraws its pictures from the saves, so thumbnails update when a city saves, not live. Most of the reverse engineering and the code were done with Claude Code, with every game method it touched exercised in-game and logged before anything was built on it. I supervised, tested and verified.

Credits and third-party code

- gzcom-dll by simmaster07 and contributors (LGPL 2.1). Lets the plugin register itself with the game.
- sc4-dll-utilities by Null 45 (LGPL 2.1). Game version detection, logging and reading the INI file. His published plugins set the conventions this one follows.
- Windows Implementation Library by Microsoft (MIT). Used by sc4-dll-utilities.
- Monocypher by Loup Vaillant (CC0 or BSD 2-Clause). Signs the traffic between two launchers.

Source code and license

The source code is on GitHub: https://github.com/mbworth/sc4-region-sync. SC4RegionSync is licensed under the MIT License. The vendored libraries keep their own licenses.
