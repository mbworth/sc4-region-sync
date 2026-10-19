# Design study: save concurrency across two PCs

Date 2026-10-19. Inputs: DESIGN.md, ARCHITECTURE.md, research/save-641.md, research/deals-641.md, research/record-connections-641.md, PLAN-SAVE-LOCK.md, PLAN-WATCH.md, PLAN-LAUNCHER.md, OPEN-QUESTIONS Q3, Q4, Q9, Q13, Q14, Q17, src/SaveHook641.cpp, src/SaveLock.cpp, src/TileLock.cpp, src/InstanceStatus.cpp.

Player constraints: no shared network drive, no SMB setup, no junctions to UNC paths. One PC is the **host** and keeps the region on its local disk. The launcher on the **guest** points at the host, pulls the region, keeps it in sync and pushes the guest's saves back. The UX stays simple even if the code gets harder.

> **What PLAN-LAN slice 1 changed (2026-10-19).** No Reclaim: the reconcile picker is deferred to slice 2, and slice 1 keeps and flags guest-saved tiles. The archive lives outside Regions (`RegionSyncLAN\archive`). CPace pairing replaces PBKDF2. No status-file heartbeat (liveness is the mirrored status file; the wire has a `PING` keepalive). `SaveHook641` had no `onExit`, so there are two callbacks, `onSaving` and `onSaved`. The save counter lives in `_saves\<region>\x_z.saves`, not the status file. Where this study disagrees, `docs/PLAN-LAN.md` wins.

## 0. Summary

- Live data (snapshots, status files) crosses between launchers over one TCP channel. Each PC keeps a local store; the plugin does not change for this part.
- Every `.sc4` has exactly one **owner** at a time. Owners are held as tile **leases** by the host launcher. At rest the host owns every tile. A guest takes a lease when one of its instances opens a tile and gives it back after its last save has been committed on the host.
- Only the owner's saves leave a PC. The guest launcher pushes after every save. The host commits a push only if it was based on the host's current generation of that file; otherwise the push becomes a conflict copy. The host's other saves (its own tiles, plus the neighbor writes into tiles nobody has open) are pulled by the guest into its local mirror whenever that tile is not open on the guest.
- **No cross-host save lock is needed.** No file is shared between disks, so the save-641 races stay host-local, where the existing kernel mutex covers them. The launcher becomes a third user of that mutex, so its file replacements never overlap a local save or reload. The tile lease is the cross-host guard.
- Option (3) is used in one place: the neighbor write into a tile owned by the other PC is ignored by default, and in slice 3 it is suppressed in the plugin (deny handle). The owner's live record reaches the other PC through the snapshot. Experiment 1 decides whether suppression changes vanilla semantics.

## 1. Rejected: one shared Regions folder

Both games on one SMB folder would need the player to create a share, set credentials and (on the guest) a directory symlink to a UNC path, which is exactly the setup the player ruled out. It also stretches save-641's races across a network. The `OPEN_ALWAYS` stub window grows from microseconds to SMB round trips. Protection from sharing violations depends on the server enforcing share modes and on handles surviving SMB durable-handle reconnects. A partition in the middle of a save can let the peer's neighbor write create the empty stub after the cross-host lock lease expires. The game's behavior with a networked Regions folder is unknown. Two outside findings settle it: nobody has reported running SC4 live on a network Regions folder, and the prior art for multi-PC regions is file-sync tools that copy region folders between turns, never live. Our design is the live version of that prior art: copy whole files, one writer per file.

Built-in Windows file sharing created by the launcher itself is not realistic with zero user steps either. `New-SmbShare` needs elevation, "File and Printer Sharing" is often blocked by the firewall profile, the guest needs credentials for a host account (a Microsoft-account sign-in complicates this), and Windows 11 24H2 blocks insecure guest access and requires SMB signing. Every one of those is a dialog or a settings page.

## 2. Main design: per-PC Regions, launcher replication, tile ownership

### 2.1 Topology

```
 HOST PC                                          GUEST PC
 Documents\SimCity 4\Regions\<region>\  (truth)    Documents\SimCity 4\Regions\<region>\  (mirror + owned tiles)
 Plugins\RegionSync\<region>\*.snapshot (local)    Plugins\RegionSync\<region>\*.snapshot (local)
 Plugins\RegionSync\_instances\*.status            Plugins\RegionSync\_instances\*.status
 Plugins\RegionSync\_lan\leases                    Plugins\RegionSync\_lan\leases   (written by its launcher)
 instances 1..n  (plugin unchanged in kind)        instances 1..m
 launcher: HOST mode  <======== TCP, one connection, HMAC per message ========>  launcher: GUEST mode
   lease table, generations, commit, conflicts                                   journal, push queue, pull
```

On each PC the plugin keeps doing what it does today against local folders: publish and pull the local store, Q17 store watch, Q17 region watch, local save mutex, local tile mutex, status file. The launcher is the only thing that crosses the network, and on its own PC it is the only process other than the game that writes into Regions.

### 2.2 Transport, discovery, pairing

- **Transport.** Plain Winsock TCP from the launcher, length-prefixed key=value messages (same text style as snapshots), files sent in chunks with a SHA-256 at the end. No http.sys, because it needs `netsh http add urlacl` (admin). The network runs on a worker thread that posts results to the UI thread. This is the launcher's first thread; the game plugin stays single-threaded.
- **Firewall.** The first time the player clicks **Host** the launcher listens on a fixed TCP port plus one UDP port. Windows shows its "allow access" prompt once (an admin click on most gaming PCs), and the rule covers TCP and UDP for the exe. The guest only connects out, and its broadcast's reply comes back through the stateful firewall, so the guest gets no prompt. If the host's network profile is Public the prompt's default rule does not apply. The launcher reads the profile (`INetworkListManager`) and says so in plain words. Realistic floor: one prompt on the host and nothing on the guest.
- **Discovery.** The guest's **Join** broadcasts a UDP query. Hosts reply with their PC name, regions and launcher version. A text box takes a name or address when broadcast is blocked (Wi-Fi client isolation).
- **Pairing.** The host shows a 6-digit code while it is accepting guests. The guest types it once. Both derive a key from the code and the host's random id, and every message carries an HMAC-SHA256 with a counter (BCrypt, no new dependency). This keeps a stray LAN device from writing into the host's Regions. It does not protect against a sniffer who brute-forces 6 digits offline, which is acceptable for a home LAN; say so in the README.
- **Handshake checks.** Same DLL version. Same exe version (641). A **Plugins manifest hash** (relative path plus size of every file under Plugins, our own DLL and INI excluded). A city saved with a lot from a plugin the other PC lacks loads there with missing buildings, so a mismatch is a red banner that lists the differing files. It is not a hard block, because the player may know better.

### 2.3 First sync of a region

1. The guest picks a region from the host's list. The host sends a manifest: every file in the region folder (`*.sc4`, `region.ini`, `config.bmp`, anything else) with size, SHA-256 and the host's generation number.
2. If the guest already has a folder of that name that it did not get from this host (the launcher's own state file records the pairing; nothing goes into Regions), the launcher renames it to `<name> (backup yyyy-mm-dd)` after asking. The folder name must stay identical, because the store path, save mutex and tile mutex names derive from it.
3. Downloads go to a staging folder on the same volume as Regions (launcher-owned, outside Regions). Each file is verified, then moved into place with `MoveFileExW(REPLACE_EXISTING)` under the local save mutex (see 2.6). A region that is open in a guest instance during the first sync is refused ("close the region view first"), because the region view has the old file set loaded.
4. Then the store and `_instances` mirroring starts (2.8), and the guest launches instances as today.

### 2.4 Ownership rule and leases

- **Rule.** A tile's file has one owner. The host owns every tile that is not leased. A guest owns a tile while it holds the lease. Only the owner's changes to a file propagate. Anything else that changes a file on a non-owning PC is a local artifact and is overwritten by the next pull.
- **Lease table.** Kept by the host launcher, saved to disk (launcher state dir) on every change: `tile -> {owner host, granted at, base generation, state: active | releasing | offline}`.
- **Taking a lease (guest).** The player opens tile G in a guest instance as usual. The plugin writes `state=city regionX regionZ` in its status file (exists today). The guest launcher sees it and sends `lease G base=<sha256 of the local G.sc4>`. The host grants only if G is not leased, no host instance has G open (host status files) and the base hash equals the host's current G. It answers within a round trip. The guest launcher writes `_lan\leases` (`x_z=granted|denied-open|denied-stale|pending`). The plugin reads it at `PostCityInitComplete` and on `SimNewDay` until the answer is final.
- **Denied** behaves like today's same-tile conflict, made cross-host: `conflict=1` in status, publish refused, nothing pushed. On top of that, one `SC4NotificationDialog`: "This city is open on HOST-PC (or your copy is out of date). Exit without saving; changes here will not reach the host." (One dialog is the only UI the plugin adds.)
- **Host-side opens.** Host instances take an implicit lease at city load through the same check. If the tile is leased to a guest, the host instance goes into conflict mode and gets the same dialog. The host's save of that tile still lands on the host disk (the plugin cannot stop it), and the generation check in 2.5 turns the guest's later push into a conflict copy, so nothing is lost silently.
- **Giving it back.** The guest's status goes to `region` or the process is gone, and every save of G has been committed: the launcher sends `release G`. A lease with unpushed saves is never released.
- **Change of owner** is release plus a later grant with a fresh base check. Nothing else moves ownership except the host player's explicit **Reclaim** (2.7).

### 2.5 Save push and pull timing

**Guest push (owner = guest).**

1. The game saves G through the slot-15 wrapper (unchanged) under the local mutex. `onExit` adds one line of work: the director rewrites its status file with `saveSeq=<n> saveTile=x_z saveOk=<result>`.
2. The guest launcher (change handle on `_instances`) sees `saveSeq` advance. It takes the local mutex with a try-lock loop (100 ms retries, never blocking the game) and copies `G.sc4` to staging. The file is opened `GENERIC_READ`, `FILE_SHARE_READ | FILE_SHARE_DELETE`, so the game's next rename is never blocked (the game's own handle shares READ). Then it releases the mutex, hashes and uploads `push G base=<gen> sha=<...>`.
3. The host commits if `base == current gen of G` and the lease is the guest's. It moves staging over `Regions\G.sc4` under the host's local mutex, sets `gen+1` and records the hash. Otherwise it puts the file in `Conflicts\` (launcher folder, **never** in Regions: two files with the same tile position in Regions would break the region) and marks the tile `conflict`.
4. The guest journal records `lastCommitted G = gen, sha`. Typical latency: a 20 MB city on gigabit is well under a second after the save. On Wi-Fi it is a few seconds.

**Host-side changes, pulled by the guest.** The host launcher watches its Regions folder (same signal-only change handle as Q17). For a changed file it waits for two stable probes and rehashes. If the file is not leased, a changed hash gets `gen+1` and an announcement. If it is leased to the guest, the change comes from a host neighbor write into the guest's tile. It is **ignored**: no new generation, and the guest's next push overwrites it. Without this rule every host save next to a guest tile would turn the guest's next push into a false conflict. The guest pulls an announced file when no guest instance has that tile open (status files) and it is not a tile the guest owns. Same staging and replace-under-mutex path as the first sync. In the guest's region view, Q17's RegionWatch then reloads the tile (hover population updates; the picture updates on the next region-view entry, as Q17 found).

**Neighbor writes on the guest.** Guest saves write the guest's view of neighbor records into its local copies of host-owned tiles (vanilla step 10, the files are free locally). Under the ownership rule these are artifacts. The guest launcher sees the hash differ from the host's current generation and re-pulls when the tile is idle. Cost: one re-download per adjacent tile per guest save. Slice 3 removes the cost by suppressing those writes (2.9).

### 2.6 Local serialization: the save mutex gains a third user

The launcher opens `Local\SC4RegionSync.SaveLock.<region>` with `CreateMutexW` (same session, same name; it creates the mutex if no instance has the region open). It only ever try-locks, like RegionWatch: zero wait, retry on a 100 ms timer, so it can neither stall a game save nor flip the plugin into its "timed out, going ahead unlocked" mode. Holding the mutex across a `MoveFileExW` (milliseconds) or a local copy (under a second for 50 MB) fits inside the observed save holds (0.6 to 12 s). Plugin side: `SaveLock: opened ... peer=1` now also appears with only one instance running, because the launcher holds the object. Harmless; documented.

### 2.7 Crash, disconnect, reclaim

| Event | Behavior |
|---|---|
| Guest game crashes | Unsaved play is lost (vanilla). The guest launcher reaps the status file (exists today) and releases the lease if everything has been committed. |
| Guest launcher closed or crashed while the guest plays | The plugin checks `_lan\launcher.alive` (mtime, touched every 5 s by the launcher) on the month tick and logs `LAN: launcher not running, saves are not being sent to the host` plus one notification. Saves stay on disk. The next launcher start reconciles. |
| Network drop, either side | The guest keeps playing and saving. The push queue is kept in the journal. Remote snapshots in each local store are **kept** for a 120 s grace, so the neighbor looks frozen, as in vanilla. After that the launcher deletes them, and the mirror takes the dead-publisher path (history dropped, no demolition, Q13 step 4). Both launchers show a banner. On reconnect: handshake, queued pushes in order, snapshots resume. |
| Guest PC power loss | Saves on disk are complete (temp file plus rename). The lease stays with the guest and shows as "offline" on the host. At restart, reconcile pushes anything newer than `lastCommitted`. |
| Host PC power loss | The guest keeps playing and queues. Commits are atomic renames from a verified staging file. A half-written staging file is discarded at restart, and the lease table is reloaded from disk. |
| Host wants a tile leased to an offline guest | **Reclaim** in the host launcher, behind a warning: "GUEST-PC may have saves of G that never arrived. Reclaim anyway?" The lease drops and the host plays G from its current generation. When the guest returns, its unpushed save is based on an older generation, lands in `Conflicts\` on both PCs, and the player chooses. Leases never expire on their own, because a timer cannot tell a crashed guest from a slow one, and expiry is the only way a fork could happen silently. |

**Reconcile at launcher start (both roles).** Hash every region file. Guest: an owned tile whose hash differs from `lastCommitted` is pushed. A non-owned file whose hash differs from the host's current hash is pulled. Host: a file whose hash differs from the recorded one and that is not leased gets a new generation. Leftover `X.sc4_` files (save-641 failed renames) are reported, never auto-fixed.

### 2.8 Live data over the same channel

- **Snapshots.** The launcher forwards a local `*.snapshot` only if its `writerHost` is this PC (no echo loops). The receiver writes it into its local store through temp plus rename. The plugin's Q17 store watch then pulls on the next sim day. When a local writer's snapshot is deleted (city shutdown, or the plugin's own liveness reap), the delete is forwarded. A remote writer's liveness reads as "unknown, apply" in WriterIdentity (other host), so cleanup of remote snapshots is the launchers' job: forwarded deletes, plus the disconnect grace above.
- **Status files.** Forwarded the same way into `_instances` with the remote host in `host=`. The plugin's reap rule already skips other hosts. The launcher lists remote rows with a host column, read-only (no raise, no End Task).
- **Leases** go into `_lan\leases` on both PCs, so the plugin needs no network code.
- Clock skew does not matter: ordering comes from generations and the single host authority, never from wall-clock comparison across PCs.

### 2.9 Where option (3) is folded in

**Slice 3, after experiment 1.** In the save wrapper's `onEnter` the director opens each adjacent tile's `.sc4` that this PC does not own, with `GENERIC_READ`, `FILE_SHARE_READ`. `onExit` closes them. The game's neighbor write then fails silently with a sharing violation. That is the vanilla path observed in Q4 step 5 whenever a neighbor's city is open, so no code is patched. Locally the files always exist (only the launcher replaces them, and it holds the same mutex), so the `OPEN_ALWAYS` stub cannot happen. Effects: the guest mirror stays byte-identical to the host (no re-pulls after guest saves), and the host disk never holds the host's view of a guest-leased tile (less to ignore). The owner's own record reaches the other PC live through the snapshot and on disk through its own save.

Why this is likely not a semantic loss: deals-641.md section 2 says a city learns of shared deals at `Init` by cloning each neighbor's local deals **from that neighbor's own record**, which the neighbor's own save writes into its own file. The neighbor write replaces that record with the writer's view. So the PLAN-SAVE-LOCK statement "reloaded cities depend on the neighbor's saved copy" may describe the neighbor's own save, not the cross write, and Q9 step 4's amount swap may even be caused by the cross write. That is a hypothesis; experiment 1 tests it.

Full option (3) (no `.sc4` ever crosses, records only through the store) is rejected. Tiles would be bound to one PC forever, region views would diverge, and loading next to an offline neighbor would need persisted records plus a re-implemented deal clone (record `+0x18C` has no setter).

## 3. Data-loss timelines (main design)

| # | Timeline | Outcome |
|---|---|---|
| T1 | 14:00:00 guest saves G. 14:00:00.3 guest PC loses power while uploading. 14:05 guest restarts, launcher reconciles. | G.sc4 on guest is complete. Lease still the guest's. Push commits at 14:05. **No loss.** |
| T2 | T1, but at 14:02 the host player reclaims G, plays and saves (gen N to N+1). 14:05 guest pushes based on N. | Guest's file goes to `Conflicts\`, banner on both PCs, the player picks one version. **One branch is discarded by the player's choice, never silently.** Cities cannot be merged. |
| T3 | Guest plays G for 20 minutes without saving, game crashes. | Unsaved play lost, as in vanilla. Lease released by the reap. |
| T4 | The host has G open. At 15:00 the guest opens G too. | Lease `denied-open`, dialog in the guest, `conflict=1`, nothing published or pushed. If the guest keeps playing and saves anyway, that session stays on the guest (and is overwritten by the next pull once the tile is idle there). **The guest's session is lost if the player ignores the dialog.** The launcher offers "keep as conflict copy" before the pull overwrites it. |
| T5 | 15:00:00.0 the host saves G (unleased), gen N+1. 15:00:00.4 the guest clicks G before the pull lands and loads gen N. | Lease `denied-stale`, same as T4. The pull lands as soon as the guest exits the tile. **No loss** if the player follows the dialog. |
| T6 | Upload cut halfway, or a corrupt chunk. | SHA mismatch, staging discarded, retried. **No loss.** |
| T7 | The host's lease table file is deleted or the launcher is reinstalled. The host player opens G (leased to the guest), plays, saves. | Becomes T2 on the guest's next push. **No silent loss**, conflict copy. Mitigation: the table lives next to the launcher's journal, and losing it shows "lease table missing, all tiles assumed at host" at startup. |
| T8 | Host power loss mid-commit. | `MoveFileExW` is atomic on NTFS: old or new file. The staging file is reverified at start. **No loss.** |
| T9 | Host region edit (MoveCity, delete or found a tile, terraform, rename region) while a guest holds leases. | Region layout differs between PCs. The guest's push may describe a tile that moved. **Possible loss.** Rule: the host launcher warns when `config.bmp`, `region.ini` or the set of `.sc4` files changes while any lease is out, and the guest must re-sync the whole region with no leases. Region editing while a guest is connected is unsupported, and the README says so. |
| T10 | A guest neighbor write lands in the guest's local copy of host-owned H. | Overwritten by the next pull (or prevented in slice 3). Only the guest's view of H's record is dropped. **No player data lost.** |

Compared with the current one-PC setup, nothing here adds an `OPEN_ALWAYS` stub path. Every new loss path needs the player to override a dialog or reclaim a tile, and then the losing version still survives in `Conflicts\` unless the player deletes it.

## 4. What the player sees

- **Host:** a **Host** toggle (one firewall prompt the first time), the pairing code, connected guests with PC name, version and Plugins-match light. The tile table: tile, city, owner (Host, GUEST-PC, or "GUEST-PC offline"), generation, last commit time, and Reclaim on offline rows.
- **Guest:** **Join** with the list of found hosts or a name box, the code once, the region pick, a progress bar for first sync. Then the usual instance list with a host column, and a status line: "Synced with HOST-PC, last push 14:00:01 (G, 18 MB)". Banners: host unreachable (saves kept and queued), Plugins mismatch, conflict copies waiting.
- **In game:** nothing new unless a tile is denied (one notification) or the launcher is missing (one notification).

## 5. Operator setup

1. Both PCs: game 1.1.641, the same Plugins folder (the launcher checks), `Setup-MultiInstance.ps1 -Setup` as today (local junctions only).
2. Host: start the launcher, click **Host**, allow the firewall prompt on a Private network.
3. Guest: start the launcher, **Join**, pick the host, type the code, pick the region, wait for the sync, launch.

No shares, no credentials, no UNC paths, no INI edits.

## 6. What changes in existing components

| Component | Change |
|---|---|
| `SaveHook641` | None in slices 1 and 2. `onExit` is reused by the director. |
| Director (`RegionSyncDllDirector.cpp`) | In `onExit`, status rewrite with `saveSeq/saveTile/saveOk`. Read `_lan\leases` at city load and on `SimNewDay` until final, `denied-*` sets the existing conflict path and shows one notification. Month-tick check of `launcher.alive`. Slice 3: deny handles on non-owned adjacent files in `onEnter`/`onExit`, setting `SuppressForeignNeighborWrites=1`. About 150 lines. |
| `SaveLock` | No code change. Adds a documented external user (the launcher, try-lock only). `peer=1` with a single instance is expected. |
| `TileLock` | Unchanged (local). The cross-host guard is the lease, consumed through the existing conflict path. |
| `WriterIdentity` / liveness | Unchanged. Remote writers stay "unknown, apply", and launchers own remote cleanup. |
| `InstanceStatus` | Schema 2: `saveSeq`, `saveTile`, `saveOk`, `lease`. The reap rule is unchanged (own host only), and remote rows are never reaped by the plugin. |
| Store watch / RegionWatch (Q17) | Unchanged. Launcher-written snapshots and replaced `.sc4` files trigger them like a local peer would. RegionWatch's two-probe stability check already tolerates a replace in progress. |
| Launcher | The bulk of the work: TCP service and client on a worker thread, UDP discovery, pairing and HMAC, handshake checks, manifests and SHA-256, staging and commit, lease table, guest journal and push queue, store and status mirroring, reconcile, conflict folder and UI, host column, banners. About 2,000 lines on top of today's 2,155. |
| Ground rules (CLAUDE.md) | "Never write to `.sc4` or Regions" stays for the plugin. Add: the launcher may replace whole files in Regions, only files it replicates, only atomically from verified staging, only under the save mutex, never a tile open on that PC. |
| Docs | DESIGN (store per PC, launcher channel, ownership), ARCHITECTURE (topology, launcher threads, lifecycle rows), PLAN-LAN.md, Q18. |

Complexity: the plugin grows by about 1 % and gains no game-facing call except one notification dialog. All the risk sits in the launcher, which is outside the game process and can be unit-tested on Linux for the manifest, generation, lease and journal logic (keep those game-free and Windows-free, as `InstanceStatus` is).

## 7. Unknowns that need the game

The experiments below cover these unknowns:

- whether suppression changes reload semantics (deals, connections, edge stubs);
- whether a replaced neighbor file under a running game (city or region view) is safe;
- whether a guest-saved file loads identically on the host;
- the behavior when Plugins differ;
- whether reading the own `.sc4` with `SHARE_DELETE` right after a save never blocks the next save;
- the firewall prompt's real behavior on a clean profile.

## 8. Recommendation

Build the launcher-replicated design (section 2) in three slices:

1. **Live only.** Discovery, pairing, handshake, first sync (host to guest, read-only), snapshot and status mirroring. The player can already play two PCs live. Guest saves stay local and are flagged "not sent", so tiles should be played on the host or treated as scratch on the guest.
2. **Ownership.** Leases, push and commit, pulls of host changes, conflict copies, reconcile, Reclaim. This makes the guest's saves real.
3. **Suppression and polish.** Deny-handle suppression if experiment 1 passes, the Plugins-diff list, conflict chooser.

Reasons: it meets the no-share constraint with one firewall click. It keeps every `.sc4` single-writer per disk, so the save-641 races never cross the network and no cross-host lock is needed. Every failure ends in "queued", "denied with a dialog" or "conflict copy", never in a silently lost or stub file. The game plugin barely changes and stays single-threaded with no network code. And it follows the prior art (copy whole region files) live instead of between turns.

## 9. Experiments (proposed Q18, house style)

## Q18. Can two PCs play one region with the launcher replicating saves and tile leases?

Plan: `docs/PLAN-LAN.md`. Host the main PC, guest a second PC. Region New York, BumperVille (4,6) and TuggersVille (6,6). Back up the region folder on both PCs first. File times: `gci <region> -Filter *.sc4* | % { '{0} {1:HH:mm:ss.fff} {2}' -f $_.Name, $_.LastWriteTime, (Get-FileHash $_.FullName).Hash.Substring(0,12) }`.

1. **Suppression keeps vanilla reload semantics (one PC, before any LAN code).** Two instances, a power deal and a water deal across the shared edge, `SuppressForeignNeighborWrites=1` with both tiles treated as foreign. Save both, exit both, reopen both, `RegionSyncDump`. Pass: in both cities the deal list (type, amount, expiry), connection count and edge stubs equal the pre-save dump, with no duplicates. On each save the neighbor's `.sc4` time is unchanged. Fail: a deal or connection missing or doubled after reload. Then the cross write carries something load depends on, and slice 3 is dropped (ignore-and-re-pull stays). Record which.
2. **Deny handle is silent.** Ctrl+S with suppression on, the neighbor in the region view (where vanilla's write would land). Pass: the save logs `released ... ok=1`, the neighbor's `.sc4` time and hash are unchanged, no dialog. Fail: a dialog, `ok=0`, or a changed neighbor file.
3. **Copy after save never blocks the next save.** A test tool (or the launcher) copies the own `.sc4` with `SHARE_READ|SHARE_DELETE` in a loop while the player Ctrl+S's five times. Pass: all five `ok=1`, and each copy's hash equals the file as of its save. Fail: any `ok=0` or a torn copy.
4. **Replacing a neighbor file under a running game.** The launcher replaces `City - TuggersVille.sc4` (from another backup) under the mutex: (a) while BumperVille is open, (b) while instance 1 is in the region view. Pass: (a) no crash and no log change until the next load. (b) `RegionWatch: 1 tile(s) due`, `Reload (6,6) ... recordPtrChanged=1`, hover population changes. Fail: crash, or a `SaveLock` timeout in the game.
5. **Mutex sharing with the launcher.** The launcher holds the try-lock for 3 s (debug button) and the player presses Ctrl+S. Pass: `save: acquired waited≈3000 ms`, then a normal release. The launcher never logs a blocking wait. Fail: a timeout line in the game log.
6. **Firewall and discovery.** Clean host profile, Private network: one prompt at the first **Host**. The guest's **Join** lists the host within 2 s and no prompt appears on the guest. Switch the host to Public: the launcher shows the profile message, and connecting by name fails with a clear error. Fail: a prompt on the guest, or a silent failure.
7. **Cross-PC file fidelity.** First sync, then on the guest open BumperVille, `RegionSyncDump`, and compare with a host dump of the same file. Pass: population, budget, deals and connections equal, and the region thumbnail renders. With one plugin removed on the guest: the launcher banner lists it. Record what the city shows (missing lots?), because that decides whether the mismatch should become a hard block.
8. **Lease and push.** The guest opens TuggersVille: the guest log shows `LAN: lease (6,6) granted`, and the host table shows the guest. The guest saves. Pass: within 5 s the host's `City - TuggersVille.sc4` hash equals the guest's, gen+1, and the host's region view (RegionWatch) reloads it. The guest exits the tile and the lease shows released. Fail: hash mismatch, or a lease still held with nothing pending.
9. **Same tile on both.** The host has BumperVille open, and the guest opens it. Pass: `denied-open`, dialog, `conflict=1`, the guest's Ctrl+S produces no push, the host file is unchanged. Fail: any push, or no dialog.
10. **Stale load.** The host saves DumpsVille (unleased). Within 1 s the guest opens it. Pass: `denied-stale` if the pull had not landed, otherwise `granted` with the new hash. Never `granted` with an old hash.
11. **Ignored host neighbor write.** The guest holds TuggersVille, and the host saves BumperVille (adjacent, so its step 10 writes into the host's TuggersVille file). Pass: the host launcher logs "leased tile changed locally, ignored", no new gen. The guest's next push commits without conflict. Fail: a conflict on that push.
12. **Disconnect.** Unplug the guest mid-play, save twice, wait 3 minutes, reconnect. Pass: the guest keeps running, the banner appears, and after the 120 s grace the host log shows the guest's snapshot removed (`history dropped`, no demolition). On reconnect both queued pushes commit in order, the final host hash equals the guest's, and snapshots resume (`Pull (store change)` within a sim day). Fail: lost push, demolition, or a crash.
13. **Guest power loss after save.** End-task the guest launcher right after a save (simulates the PC dying before upload), then start it again. Pass: reconcile pushes the save, gen+1. Then the reclaim variant: kill the launcher after a save, Reclaim on the host, host save, restart the guest launcher. Pass: the guest's file is in `Conflicts\` on both PCs and the host file is unchanged by it.
14. **Live latency.** Both PCs in city at normal speed for 10 minutes. Pass: every peer month tick produces `Pull (store change)` on the other PC within one sim day, with no Warning lines. Write down the end-to-end time from publish on one PC to apply on the other.

**Observed.** (empty)

## 10. Decision 2026-10-19: reconcile on connect replaces Reclaim

Player direction: both PCs may play alone while apart, and divergence is settled by a person when they reconnect, not by a Reclaim button.

- **Offline play is allowed on both PCs on any tile.** Leases apply only while connected. While apart, each PC plays its local copy.
- **A tile counts as changed on a PC only if that PC saved it with the city open.** The plugin stamps a per-tile save counter (the save hook sees every save) into the status file; the launcher keeps a per-tile `(playedGen, hash)` journal from that. File mtime and hash alone never decide, because the host's own saves rewrite its copies of neighbor tiles (Q4) and would look like divergence.
- **On connect, per tile:** identical, nothing. Changed on one side only, fast-forward: the other PC pulls it, no question asked, a line in the launcher log. Changed on both sides, the picker.
- **Picker:** one row per divergent tile with both sides: city name, population, sim date, last save time, which PC. Default suggestion is the later sim date. Buttons: pick per row, "keep all from host", "keep all from guest". The losing copy is archived under `Conflicts\<date>\` on both PCs, never deleted. After the choice the winner becomes the host's generation and the other PC pulls it.
- **While the picker is open,** no leases are granted for the divergent tiles and the plugin's tile guard shows them as held, so neither game opens one mid-decision.
- **Reclaim is removed.** The host never takes a tile back by itself. An offline guest's lease simply lapses to "offline, changed on guest" and goes through the picker on reconnect.
- **Replaces** the T2 and T9 timelines in section 3: a Reclaim-then-return case becomes a two-sided change and lands in the picker; region edits while apart are two-sided changes to `region.ini` and `config.bmp`, shown as one extra picker row ("region layout") rather than unsupported.
