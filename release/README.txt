SC4RegionSync 0.0.0
===================

Play several cities in one SimCity 4 region at the same time, each in its own
copy of the game, and have each city see the others' growth, neighbor
connections and deals while you play. One PC, or two PCs on the same LAN.

This is an early release. It has been played on a Steam install with up to
three instances, and across two PCs with a Steam host and a GOG guest. Back up
your Regions folder before you start. Report problems with the SC4RegionSync.log from each instance.

Requirements
------------
- SimCity 4 Deluxe 1.1.641 (Steam or GOG digital release). Disc versions and
  the macOS port do not load DLL plugins.
- Visual C++ 2022 x86 redistributable (the same one every other DLL plugin
  needs): https://aka.ms/vs/17/release/vc_redist.x86.exe
- Windows 10 or 11.

What is in this folder
----------------------
SC4RegionSync.dll          the plugin
SC4RegionSync.ini          its settings (defaults are fine)
SC4RegionSyncLauncher.exe  sets up, starts and lists instances; hosts or joins a LAN peer
LICENSE                    MIT
README.txt                 this file

Install
-------
1. Copy SC4RegionSync.dll and SC4RegionSync.ini into
   Documents\SimCity 4\Plugins (the game install's Plugins folder also works).
2. Copy SC4RegionSyncLauncher.exe anywhere, for example
   C:\Users\<you>\SC4RegionSyncLauncher\. It writes launcher.ini next to itself.
3. Start SC4RegionSyncLauncher.exe, set the game exe in Settings, then click
   Setup on the toolbar once. It shows what it will do, then creates
   Documents\SimCity 4 Instances\Instance1..N, each with its own preferences
   and Albums and junctions back to your real Regions and Plugins. Nothing is
   copied and nothing in your SimCity 4 folder is changed.

   Steam: Setup also offers to create steam_appid.txt (the single line 24780)
   next to SimCity 4.exe so a second copy does not relaunch through Steam.
   That folder is under Program Files, so Windows asks for administrator
   approval; decline and you can create the file by hand instead. Steam must
   be running when you play.

   GOG: the GOG installer marks SimCity 4.exe to run as administrator. The
   launcher starts it without that. If the game still will not start from the
   launcher, start the launcher itself with Run as administrator.

Play
----
1. In the launcher, open Settings on the toolbar and set the resolution and
   windowed mode.
2. Click Launch for each instance you want. The status bar estimates how many
   more fit in RAM and CPU; red refuses, amber only warns.
3. Open a different city tile in each instance. The plugin publishes each
   city's regional record every sim month and on save, and the other
   instances pick it up within a sim day. Neighbor connections and deals made
   in one city appear in the adjoining one.
4. Save normally. Saves are serialized across instances so two copies never
   write the same file at once. If two instances open the same tile, the
   second one stops syncing until that city is closed.

Grid on the toolbar shows every instance as a live thumbnail; click one to
raise it. A game that exited badly shows up as an orphan after 90 s with an
End Task button.

Two PCs
-------
Both PCs need this same release installed and the same Plugins (NAM, CAM and
any other mods): copy the host's Plugins folder to the guest, leaving out its
RegionSync folder. One PC hosts the region, the other joins and gets a copy
of it.

- Host: click Host. Allow the firewall prompt on Private networks (a Public
  network profile blocks the guest). The strip shows a 6-digit code.
- Guest: click Join, pick the host (or type its address), enter the code,
  Connect, then pick the region. The host's region is copied over; your own
  folder of that name is archived to Documents\SimCity 4\RegionSyncLAN\archive
  first. The copy waits until no SimCity 4 is running on the guest.
- After that both PCs mirror live. The code is single-use; once paired the
  launchers reconnect on their own. Pair only with your own PC on a network
  you trust: traffic is signed, not encrypted.
- Guest saves stay on the guest in this release (the strip flags the tile
  "not sent"). Re-sync replaces them with the host's copy.

Ports: TCP 47641 and UDP 47642 (Port under [Lan] in launcher.ini). Do not
forward them on your router. The host is only listed in Join when both PCs
are on the same subnet. Across subnets or VLANs, type the host's address in
Join and let the guest reach the host on TCP 47641.

Cheats (Ctrl+X)
---------------
RegionSyncStatus   instance id, tile, counts, store path
RegionSyncPublish  publish this city's record now
RegionSyncPull     pull the other instances' records now
RegionSyncMirror   create the other instances' connections and deals now
RegionSyncSave     fast-save
RegionSyncDump     write the whole region table to the log

Settings
--------
Everything in SC4RegionSync.ini has a sane default. The ones worth knowing:
  PublishEveryMonths / PullEveryMonths   cadence, default 1
  AutoSaveEveryMonths                    default 0 (off)
  SaveLock                               keep on with more than one instance
  LogLevel                               Info; set Debug when reporting a bug

Logs
----
Each instance writes SC4RegionSync.log into its own folder under
Documents\SimCity 4 Instances\InstanceN. The launcher writes lan.log under
Documents\SimCity 4\RegionSyncLAN. Attach these when reporting a problem.

Uninstall
---------
Delete the DLL and INI from Plugins, the launcher folder, and
Documents\SimCity 4 Instances. Your Regions folder is never modified by the
plugin; the launcher only replaces region files on a guest PC during a LAN
sync, after archiving the originals.

Source and license
------------------
MIT. Source: https://github.com/mbworth/sc4-region-sync
Built on gzcom-dll (nsgomez) and sc4-dll-utilities (Null45).
