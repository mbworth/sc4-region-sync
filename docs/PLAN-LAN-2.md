# Plan: two PCs on one region, slice 2 (saves flow both ways)

**Status:** reworked 2026-11-06 (route C), pending review. Nothing built.

Inputs: `docs/PLAN-LAN.md` (slice 1, built; where this plan is silent, slice 1 holds), Q4, Q14, Q17, Q18 and Q20 in `docs/OPEN-QUESTIONS.md`, `docs/research/save-641.md`, `docs/research/deals-641.md`, `docs/research/record-connections-641.md`.

**Today** (slice 1) the host's region is copied to the guest on connect, snapshots and status files mirror live, guest saves stay on the guest flagged "not sent", and a host save reaches the guest only at the next connect with no game running there.

**Route C** (owner, 2026-11-06). Q20 showed a neighbor write changes exactly one DBPF entry of the neighbour's file (the regional record `0xCA027EDB/0xCA027EE1/0`), plus the directory `0xE86B1EEF` and header date and hole fields, and that a city loads its deal with a neighbour from the neighbour's file. So files still move whole, and a neighbor-written file is a change that travels, under a rule that makes the replace equal to the neighbor write the game would have done on the receiving PC. No plugin ledger, no reconciliation at load, no new game call, no editing of `.sc4` contents anywhere.

## 0. Increments

All three are required before the public release. Each ships on its own and keeps every slice 1 guarantee.

| # | Adds | The player has after it |
|---|---|---|
| 2a | Cross-PC tile guard (D13, D14); plugin mutex descriptor (P3) | Opening a city that is open on the other PC gives a dark city, as two instances on one PC do today. Saves behave as slice 1. |
| 2b | Two-way file sync (D1 to D12, D15 to D17), receiving rule "tile not open on that PC" from the start; plugin P1, P2 | A save reaches the other PC within seconds while connected, unless that PC has the tile open. Any difference pauses the pair until resolved. |
| 2c | Picker over the difference set (section 9) | The player chooses per city which copy to keep. Release candidate. |

Closed-game sync is not its own increment: it is only the fallback if gate step 6 fails.

## 1. Design-freeze gate

**Q20 step 1 ran 2026-11-06 and failed** (`docs/OPEN-QUESTIONS.md` Q20): discarding a neighbor write brought back a deal the neighbour had cancelled. Route C was chosen from its file diff.

**New gate**, before any launcher code: Q20 steps 2 to 7, one PC, two instances, the current plugin build, region New York (BumperVille (4,6), TuggersVille (6,6), DumpsVille (4,4); back up the folder and the store first). The operator's assistant copies files over SSH with `Copy-Item` only after a `SaveLock: ... released` line and pulls them as `make logs` does. "Dump" is `RegionSyncDump` with its deal and connection lines. "The script" is a game-free DBPF tool the assistant writes for the gate (run with `python3 -I`, game files never committed): per entry TGI, size and BLAKE2b, header fields, `ch` and `fh` (D1), and a diff of two copies.

2. **`ch` holds (A1).** After each of these, copy all three files: (i) BumperVille lit save, TuggersVille closed; (ii) the same with TuggersVille at the region view; (iii) a save after a deal change with TuggersVille; (iv) after a new road and power line across their edge; (v) an autosave (`AutoSaveEveryMonths=1` in the test INI); (vi) TuggersVille own saves twice with time passing; (vii) TuggersVille saved right after load, paused. Pass: every file parses (section 6, no fallback); every neighbour copy differs from its predecessor only in the record entry, the directory and header date or hole fields, with size unchanged; (vi) changes another entry; (vii) recorded either way. Fail: any other entry differs in a neighbor write. Fallback: none; route C stops.
3. **Save groups (A2, A3).** Both cities open with a water deal, both saved: copies B0, T0. TuggersVille to the region view; BumperVille cancels the deal and saves: B1 (own) and T1 (record-only, `ch(T1) = ch(T0)` by the script). Both exit. For each set, open TuggersVille then BumperVille, dump both, then both open one sim day (mirror lines), dump; exit without saving and restore the next set: {B1, T1} (pass: no deal on either side, also after save and reload); {B1, T0} (Q20: deal back); {B0, T1} (record-only file with the old saver file: record what the game loads and what the mirror does). Repeat the three sets with a new deal made in BumperVille. Pass: {B1, T1} matches the PC1 state; the mixed sets are recorded (they justify D1's groups). Fail: {B1, T1} wrong. Fallback: none; route C stops.
4. **Connections (A4).** As step 3 with a road and a power line built across the edge, then bulldozed. Pass: {B1, T1} matches on both sides through save and reload. Record {B1, T0} and {B0, T1}. Fail: {B1, T1} wrong. Fallback: none.
5. **Stale in-memory neighbour record (A5).** Instance 1 TuggersVille, instance 2 DumpsVille, BumperVille closed; copy BumperVille as B0. TuggersVille makes a deal with BumperVille and saves (B1); DumpsVille then saves (B2). Script: is B2's record entry equal to B0's, B1's, or neither? Exit both, open TuggersVille alone, dump; open BumperVille, dump. Repeat with a new road from TuggersVille into BumperVille. Pass: deal and road survive; D17 not needed. Loss: D17 applies.
6. **Live swap (A6 to A9).** PowerShell takes the save mutex by try-lock (`[Threading.Mutex]::OpenExisting(...).WaitOne(0)`) around each operation. (a) Copy TuggersVille's file under the lock while the player presses Ctrl+S five times 2 s apart; record the hold time for the largest file in the region. Pass: five `ok=1`, no `SaveLock` timeout, hold under 5 s. (b) `[IO.File]::Move($copy, $target, $true)` over TuggersVille's file while it is open. Pass: error 32, hash unchanged. (c) TuggersVille open; replace DumpsVille, then BumperVille, with record-only copies under the lock. Pass: no Error or Warning, game running 60 s later; at the region view `RegionWatch: N tile(s) due`, `Reload (4,6) ... recordPtrChanged=1`. (d) A loop replaces BumperVille under the lock every second, logging each move's time, while the player clicks Play on it; at least 10 loads. (e) The same, replacing TuggersVille (a neighbour) while BumperVille loads, at least 10 loads. Pass for (d) and (e): every load succeeds; each dump matches the old or the new file (record of the neighbour for (e)), checked against the swap times around `SaveCount: ... loaded from`. Fail: a move over an open city, a crash, a dialog, a dump matching neither. Fallback: 2b replaces only with no game process on that PC and the ground-rule amendment drops "tile not open".
7. **Writes outside SaveCity (A10).** At the region view: found a city on an empty tile, rename a city, delete a city; in a city: obliterate. For each, watch `SaveLock` lines and the mtimes of every region file. Record which take the save mutex. If any does not, D5's stability check covers it.

Results go into Q20; this plan is reviewed again before 2a code.

## 2. Decisions

**D1. Sync rule.** Every `.sc4` has a full hash `fh` (BLAKE2b, as slice 1) and a content hash `ch` over every entry except the regional record and the directory (section 6). Each PC keeps, per peer and region, the shared base `(ch, fh)` of every file (section 8). Against its base a local copy is `same` (`fh` equal), `record` (`ch` equal, `fh` not: a neighbor write, or a re-save with nothing else) or `save` (`ch` changed).
- **A file travels only when it changed against the shared base.** Direction is symmetric: a guest's record-only update to an unplayed city reaches the host and is never overwritten by the host's unchanged copy. "The host is the source of truth" decides ties, layout and Re-sync, nothing else.
- **Save groups.** Every file one save wrote is one group: the saver's own file plus the record-only neighbour files whose mtime falls inside that save's window (P1). Record-only members commit only with or after the saver's file; if the saver's file is held, the group is held; if it enters the difference set, the group enters with it and is resolved with it (D10). A record-only change with no covering save window (A10) is a group of one.
- **A real save travels whole** when the other copy is `same` or `record`; the receiver's record-only copy is archived and replaced.
- **A record-only update travels whole** and replaces the receiver's copy only when the receiver's `ch` equals the sender's, so the replace changes only the record, as the game's own neighbor write would there. Otherwise it is dropped (`Record <name>: dropped, saved here`).
- **Converged:** a pushed or served `fh` equal to the receiver's current `fh` moves the base and is never a rejection.
- **The host serializes.** Every change reaches the host's copy first; the host's copy after each event is the shared state, delivered to the guest. While connected, two record-only versions of one file apply in arrival order at the host, later wins, as with two instances on one PC. At connect or resume, when both copies are `record`, the host's wins.
- **Both copies `save`** with different `fh`: difference `both` (D16).
- **Reverts** (documented, no machinery): (a) apart, both PCs neighbor-wrote the same closed city N: the host's record wins at connect, and the guest's deal or connection change toward N reverts; (b) apart, one PC changed a link toward N and the other really saved N: the link change reverts (also connected, when N is open on the other PC and saves before closing, as on one PC, where a neighbor write to an open city is skipped); (c) a stale in-memory write (D17) replaces N's whole record, so another city's unrelated change to N can be lost.

**D2. File to tile, host-side data only.** The host maps a file to a tile from, in order: its own `.saves` (`file`), the plugin's tile map (P2), the host journal. A push naming a file mapped by none waits (`WAITING map`, banner `Open <region> once in SimCity 4 on this PC so sync can match its cities.`); such a file reaches the guest only with no game process there. The guest's claimed tile is never trusted. The guest uses the host's announced tile and its own map for "open here" (D15).

**D3. Commit rule.** The host commits a pushed file for tile t only if all hold, at precheck and again inside the lock (D6): (1) the push names the file the host maps to t (D2), or is a founding or rename that passes D8; (2) D1 allows it against the host's current copy (real save: host `ch` equals the base; record-only: equals the pushed `ch`; equal `fh`: converged); (3) its group's saver file is committed or in this commit; (4) t is not open on the host (D15), and for a founding or rename no game process runs there; (5) section 6 validation passes and the staged file still has the pushed hash; (6) the pair is not paused, or it is a resolution (D10). Otherwise `REJECTED` with one reason: `both`, `stale` (record-only dropped; the guest then fetches the host's copy), `base`, `paused`, `space`, `bad`; or `WAITING` (`open`, `nogame`, `map`, `lock`, `confirm`, `group`). `both` and `base` add t, with its group, to the set.

**D4. Push queue.** The guest pushes one group at a time, files in group order, from a queue holding the latest change per file. It never starts another push before the current one ends; the host drops a `PUSH` that breaks this.

**D5. Copies.** Each copy out of Regions (staging, archive, `Conflicts\`) is made in one save-mutex try-lock hold of at most 5 s, one file per hold, together with the `.saves` and tile map reads for it. The staged copy is hashed after the release. A copy that would exceed 5 s is abandoned and retried; after 3 abandons the file is not synced and the banner reads `<file> is too large to copy while SimCity 4 runs here. Close SimCity 4 on this PC to send it.` A file changed outside any save window (A10) is copied only after two reads 2 s apart give equal `fh`, file ID, size and mtime.

**D6. Replace.** Archive copy first (D5), then try-lock, re-run D1 and D3 or the delivery decision against the current copy, verify the original's file ID, size and mtime against the archive copy, stamp the staged file's mtime with the current time, one `MoveFileExW(staging, target, REPLACE_EXISTING | WRITE_THROUGH)`, release. The new mtime is required: RegionWatch and D17 detect changes by size and mtime, and a neighbor write keeps the size. A failed check drops or retries; error 32 retries, after 20 it waits for that tile's status to change.

**D7. Change detection.** Only `ch` and `fh` against the base. A failed save leaves the file and its hashes unchanged (A11). `.saves` is read for `dark`, the save window and the map.

**D8. One file per tile; new names are layout-class.** A founding (new name on an unmapped tile), a rename (new name replacing the tile's mapped file), a removal, `region.ini` and `config.bmp` are delivered only with no game process on the receiving PC. For a guest founding or rename the host checks: the tile is unmapped (founding) or mapped to `baseFile` (rename); the new name matches no host file; the pair's founding count (host journal, lasting) is under 16; the file fits the disk cap (section 4). A rename archives the old file on commit. A founded file is mapped from the push until the host's next tile map; if that puts it on a tile that already had a file, that tile is a difference `base`. Removals and layout travel host to guest only; a guest-side removal or layout change is copied to `Conflicts\` and undone at the next no-game sync, with a banner.

**D9. No gap.** The losing copy is in `archive\` or `Conflicts\`, verified, before the single move.

**D10. Resolution.** Per tile in the set, one PC's copy wins for the tile's whole group (the union of both PCs' groups for it), every file taken from the winner, so both PCs end byte-identical for those files, records included. The losing PC's copies go to its `Conflicts\` with `conflict.txt` (tile, city, reason, both `fh`, which PC). The PC whose copy loses confirms in its own launcher first. Default winner per reason: `both` the host's; `dark` the lit copy; `base` the host's file, the guest's file name moved to `Conflicts\`, no-game class. In 2b, Re-sync on the guest applies these defaults (the host confirms where its copy loses, i.e. a host dark save); 2c lets the player choose. Keep host's: the guest copies to `Conflicts\`, `GET resolve=1`, replace, `APPLIED`. Keep guest's: `PUSH resolve=1`, the host's player confirms (section 7), the host copies to `Conflicts\` and commits; No gives `REJECTED confirm` and the tile stays. Base = the kept copy. Nothing goes to `Conflicts\` until a copy loses.

**D11. Dark saves.** A real save is `dark` when the tile's latest local `.saves` has `dark=1` (P1). It stays local, is never refused, and makes the tile a difference `dark`.

**D12. Region layout comes from the host.** `region.ini` and `config.bmp` are never pushed (D8).

**D13. Cross-PC tile guard (2a).** Each launcher holds `Local\SC4RegionSync.Tile.<region>.<x>_<z>` for every tile a mirrored peer status file shows in `city` state with `conflict=0`, and closes it when that status leaves `city` state or is reaped (slice 1 reap, 10 minute disconnect rule). Accepted only for the paired region and tiles inside `config.bmp` bounds; at most 64 held. The plugin's Q14 guard then finds `peer=1` and the city goes dark. A dead launcher drops its handles, so the guard fails open; D1 and D16 still prevent an overwrite. Names are built as the plugin does (`TileLock.cpp`, `SaveLock.cpp`): `regionDirectoryName` from a local status file's `region` value, else the on-disk folder name, no case folding (A15). A race within the mirror latency leaves both lit; both launchers show "open on both PCs", and two saves become `both`.

**D14. Mutexes and elevation.** Launcher and plugin create the tile and save mutexes with `CreateMutexExW(..., SYNCHRONIZE | MUTEX_MODIFY_STATE)` and the descriptor `D:P(A;;0x120001;;;<token user SID>)S:(ML;;NW;;;ME)` (SYNCHRONIZE, MUTEX_MODIFY_STATE, READ_CONTROL for the current user only, medium label) (P3). `ERROR_ACCESS_DENIED`: the plugin goes dark on a tile mutex (Warning) and takes today's timeout path on the save mutex; the launcher treats the mutex as held. **One elevation rule:** before any replace, the launcher checks its own token and every running local SimCity 4 process (`TokenElevation`; a process whose token it cannot open counts as elevated); if any is elevated, nothing is replaced on that PC and the banner reads `No city files are replaced on this PC while <the launcher|SimCity 4> runs as administrator. Restart it normally.` Host, Join and mirroring stay allowed.

**D15. Open here.** A tile is open on a PC if a local status in city state names it, its tile mutex exists and this launcher does not hold it (or opening it is denied), or a local snapshot with a live local writer names it (slice 1 `TileOpenHere`). A file is open when its tile is open by either PC's map (D2). A city that starts loading in between holds its file with read sharing only, so the move fails with error 32 (A8).

**D16. No sync while the regions differ.** A tile enters the difference set, with its group, for `both`, `dark` or `base`. At connect the host's base is used where the journals disagree, and an `fh` mismatch in `MANIFEST` that neither side's change explains is `base`. While the set is non-empty the pair is paused:
- Stopped both ways: city files, snapshots. Status files still cross (the guard and banners need them).
- Allowed: resolution transfers (`resolve=1`) for tiles in the set, `DIFFER`, `PAUSED`, `PING`, `MANIFEST` and `CHANGED` as display data.
- Either PC keeps playing unsynced. A tile leaves the set only by resolution. Re-sync touches only tiles in the set.
- When the set empties, both sides reclassify every file as at connect: changes made on one side during the pause travel then.
- Banner on both: `Sync paused: N cities differ between <host> and <guest>. Both PCs can keep playing; nothing is sent until each is resolved (Re-sync on <guest>).` (2c: `... Choose a copy for each in Resolve on <guest>.`)

**D17. Stale in-memory neighbour records (only if gate step 5 shows the loss).** A city Y open since before a record-only update of its closed neighbour N lands (delivered, or neighbor-written by another local instance) still holds the older record and writes it back at its next save. Rule, simplest safe option: in the save wrapper, before the original save and under the save lock it already holds, the plugin compares each closed neighbour's file size and mtime with what Y loaded or last reloaded; if N changed and Y's own deals and connections with N equal what it loaded, Y reloads N's record (`ReloadCity`, already called in city view by `RegionSyncReload`, Q3) and logs `Reload (x,z) before save: neighbour file changed`. If Y also changed its link with N, nothing is reloaded and Y's write wins (revert (c), logged). If no safe reload exists, revert (c) stands with this condition: another city next to N was open, on either PC, from before the change until after its next save.

## 3. What it touches

**Plugin** (no network code, no thread, no new game call):
- **P1.** `.saves` gains `dark=1` when the save ran in a dark city, and `windowFrom` and `windowTo` (UTC FILETIME at the `saving` and `saved` callbacks), both written inside the save lock as today (`SaveHook641.cpp`).
- **P2.** Tile map `<store>\_saves\<region>\tiles.txt`, `x_z=<basename>` per established tile with a save path, rewritten at each region-view seed when it changes (`ProbeTiles` already calls `GetCitySaveFilePath`). Format in `InstanceStatus`.
- **P3.** D14 descriptor and access in `TileLock` and `SaveLock`.
- **P4.** D17, only if gate step 5 shows the loss.

**Launcher, Windows-free** (unit-tested): `Dbpf` (new), `SyncRules` (new: classification, groups, commit and delivery decisions, difference set, guard set, picker suggestion), `Frame`, `Manifest`, `Journal`.

**Launcher, Windows:** `Net.cpp` (region watch, staging, push, `GET` by hash), `Lan.cpp` (journals, commit, delivery, pause, `Conflicts\`, guard mutexes, elevation check), `LanUi.cpp` and `launcher.rc` (strip, banners, confirmations, picker).

**Must not break:** without Host or Join nothing changes; the plugin never writes `.sc4` files or Regions; the launcher replaces files only under the amended ground rule (section 13) and never edits a `.sc4`; it takes the save mutex only by try-lock, at most 5 s per hold, so a game save never times out on it; Q4, Q14, Q17 and Phase 2 mirroring unchanged apart from P3 (and P4); slice 1 frames keep their numbers; Windows-free files stay so; `make test`, `make test-launcher` pass; `make launcher` has zero warnings.

## 4. Disk and paths

- One cap per peer, 8 GiB, over `staging\`, `archive\<peerSiteId>\`, `Conflicts\<peerSiteId>\` and founded files together, plus the free-space floor (size + 10 % + 256 MiB) before any copy. At either limit transfers stop on that PC (not a pause; mirroring continues), pushes get `REJECTED space`, deliveries wait, banner `RegionSyncLAN is full. Delete old copies in <full path>.`
- Archived copies whose `fh` was a shared base are pruned after 14 days. `Conflicts\` is never pruned.
- Paths: `<LAN folder>\archive|Conflicts\<peerSiteId>\<region>\<yyyymmdd-hhmmss>-<n>\<name>`, `<name>` passing `IsValidRegionFileName`, the stamp unique, every path `\\?\`-prefixed. Every directory the launcher creates or writes into is refused if it is a reparse point. A failed copy means no replace.

## 5. Protocol

`proto` 2. A slice 1 peer is refused at `HELLO`: `The other PC runs an older SC4 Region Sync (protocol 1, this PC 2). Install the same release on both PCs.` New types are appended. Slice 1 rules hold: exact counters, MAC, field formats, drop on any violation.

| Type | Dir | Fields | Receiver |
|---|---|---|---|
| `MANIFEST` (changed) | H to G | `file.N=name\|size\|fh\|ch\|tile` (tile empty when unmapped), `base.N=name\|ch\|fh` (host base); 2c: `info.x_z=simDate\|population\|lastSaveUnix\|cityName` for tiles in the set | D1, D16 |
| `CHANGED` | H to G | `name`, `size`, `fh`, `ch`, `tile`, `group`, or `layout=1` | D1 |
| `GET` (changed) | G to H | adds `fh`, `resolve` | serve that copy, else `GONE` |
| `GONE` | H to G | `name`, `fh` | wait for the newer `CHANGED` |
| `APPLIED` | G to H | `name`, `fh`, `resolve` | only for a copy served this session; base moves |
| `PUSH` | G to H | `tile`, `name`, `baseFile` (empty for a founding), `size`, `fh`, `ch`, `group` (the saver's file name), `resolve`, `hostFh` (with `resolve=1`) | D3 precheck |
| `READY` | H to G | `tile` | guest sends `FILE` |
| `FILE` (both ways) | G to H too | as slice 1, only after `READY` | `staging\push\`, hash and `ch` checked on `last` |
| `WAITING` | H to G | `tile`, `reason` | strip only |
| `COMMITTED` | H to G | `tile`, `name`, `fh`, `ch` | base moves |
| `REJECTED` | H to G | `tile`, `reason` (D3; `confirm`, `changed` for resolutions) | D1, D10, D16 |
| `DIFFER` | G to H | `differ.N=name\|reason` | host adds to the set, sends `PAUSED` |
| `PAUSED` | H to G | `differ.N=name\|reason`; none means resumed | the guest's copy of the set |

`DIFFER` and `PAUSED` are the only carriers of the set. `resolve=1` is valid only for a tile in the set; `hostFh` must equal the host's current `fh`, else `REJECTED changed`.

**Content hash** (`Dbpf.cpp`; every field hostile): file size 96 bytes to 1 GiB; magic `DBPF`, major 1, minor 0 or 1, index 7.0; entry count at most 65536; index size equal to count * 20; index and every entry inside the file, offsets at least 96, u64 arithmetic; no two entries overlap; no duplicate TGI; exactly one record entry, at most one directory entry. `ch` = BLAKE2b-256 over `SC4RS-CH1`, then in index order for every other entry: type, group, instance, size (u32 LE), bytes. On any failure `ch = fh`, one Warning and a banner once: `<file> could not be read as a city file. It still syncs, but if both PCs change it, sync pauses until you choose a copy.` Such a file has no `record` state. The receiver recomputes `fh` and `ch` of every received file and refuses a mismatch.

**Validation and limits:** `name` and `baseFile` pass `IsValidRegionFileName` and end in `.sc4` (never `region.ini`, `config.bmp`); names resolve case-insensitively to the host's casing; `tile` canonical and inside `config.bmp` bounds; `DBPF` magic and size cap; at most 1024 `differ` lines naming manifest files; `cityName` 255 bytes printable ASCII; out-of-state frames dropped and logged. The worker interleaves `FILE` chunks of both directions with mirror and `PING` frames.

## 6. UI strings

**Host is the source of truth (guest launcher):**
- Join, before the first sync, OK/Cancel: `<host> is the source of truth for <region>. This PC takes its region layout, and when both PCs changed the same city, <host> decides. Re-sync keeps <host>'s copy of every city saved on both PCs; this PC's copy goes to RegionSyncLAN\Conflicts first. Changes made on either PC still reach the other.` (2c replaces the third sentence with `You choose which copy to keep in Resolve; Keep all from <host> is the default.`)
- Strip while mirroring: `Mirroring <region> with <host> (source of truth)`.

**Confirmations on the losing PC:** guest Re-sync `Re-sync replaces N cities on this PC with <host>'s copies. This PC's copies go to RegionSyncLAN\Conflicts first.` OK/Cancel. Host: `Replace <city> (<x>,<z>) on this PC with <guest>'s copy? This PC's copy goes to RegionSyncLAN\Conflicts.` Yes/No.

**Other:** strip `pushing TuggersVille 4/18 MiB`, `TuggersVille sent 14:02:11`, `BumperVille open on <PC>, waiting`; guard banners `BumperVille (4,6) is open on <peer>: this copy is dark; saving it pauses sync until you choose a copy` and `BumperVille (4,6) is also open on <peer> (dark there)`; pause (D16), map (D2), too large (D5), elevation (D14), full (section 4), unreadable file (section 5).

**README text:** "Pairing makes the other PC part of your region. Saves travel both ways: a city saved on one PC replaces the copy on the other, unless that PC has the city open (then it waits) or saved it too (then sync pauses for the whole region until you choose a copy). The host is the source of truth: it decides ties, its region layout wins, and Re-sync keeps its copies. The copy that loses is kept in RegionSyncLAN\Conflicts. Deals and connections between cities can revert in three cases: (1) while the PCs are apart, both PCs saved cities next to the same unplayed city: the host's version of that city wins; (2) while apart, you changed a deal or connection toward a city that was saved on the other PC; (3) a city that stayed open while its unplayed neighbour was updated can write its older view of that neighbour back when it saves. Files travel unencrypted on your network."

## 7. Journal and upgrade

**Journal**, one format both sides: guest `journal\<hostSiteId>\<region>.txt`, host `journal\<guestSiteId>\<region>.host.txt`. Lines `f.N=name|ch|fh|tile` (shared base), `d.N=name|reason|group` (the set; the host's is authoritative), host only `foundings=N`. Written with `WriteAtomic` after every change; a failed write is an Error and a banner. One builder, one strict parser.

**First connect after upgrading** (or no host journal): the guest's slice 1 `baseHash` is the base `fh`; the base `ch` is that of whichever side still has that `fh`; then D1 decides. A file with no slice 1 line whose copies differ is `base`. **Downgrade** to slice 1: it ignores `*.host.txt`, `tiles.txt` and the new `.saves` keys, rejects the new guest journal and treats it as empty, so guest-saved tiles show "not sent" and are kept.

## 8. Picker (2c)

On the guest, `Resolve` (enabled while the set is non-empty). One row per tile in the set (its group listed in the tooltip); layout is never a row; a row whose tile is open on either PC is disabled (`open on <PC>`). Suggestion is D10's default; for `both` the later sim date each PC reports, tie host, labelled `suggested from the dates each PC reports`. Decide later keeps the pause. When the last row resolves, the pair resumes.

Title `Cities that differ between PCs`. Text `Sync between <host> and <guest> is paused because these cities differ. Choose which copy to keep for each; sync resumes when all are chosen. The other copy goes to RegionSyncLAN\Conflicts on the PC that has it; nothing is deleted.` Columns `City`, `Tile`, `Why`, `<host>`, `<guest>`, `Keep`; `Why` cells `saved on both PCs`, `dark save on <PC>`, `no shared base`; cell `pop 50,037, Jun 2153, saved 14:02`, tooltip `(that PC's clock)`. Buttons `Keep all from <host>`, `Keep all from <guest>`, `OK`, `Decide later`.

## 9. Trust and limits

Slice 1 pairing trusted the host with the guest's game. Slice 2 newly allows:
- A paired guest's files replace host files under D1 and D3: any host city the host has not really saved since the shared base. A host-changed city is replaced only after the host's player confirms (D10).
- Because `ch` excludes them, a paired peer can change the regional record and the directory of any city file whose content matches, without confirmation. The launcher does not validate them.
- The guest can found cities on empty host tiles (16 per pairing) and rename its cities, with no game running on the host (D8). It cannot touch `region.ini`, `config.bmp`, non-`.sc4` names or unmapped files, or put two files on one tile.
- Saves cross the network unencrypted.
- A guest can hold host tile mutexes through its status files, so host cities open dark (D13).
- Either side can keep the pair paused indefinitely (D16).
- A malicious paired guest can hand the host's 2003 loader a hostile file, as a malicious host already can the guest. `Dbpf` reads only the index; the host cannot check that a file is a sound city or sits on the tile claimed.
- The reverts in D1.

Pairing is the trust boundary: the peer is the player's own PC. README and `docs/DESIGN.md` say this in plain words.

## 10. Experiments: Q21 (slice 2)

New question in `docs/OPEN-QUESTIONS.md`, one Observed slot per step. Loopback unless marked, then two PCs (wired, files up to 50 MiB). Logs named here (`Commit`, `Deliver`, `Record`, `Guard`, `Differ`) are added by this plan.

**2a:**
1. **GOG non-elevated [2 PCs] (A16).** The non-elevated launcher starts the GOG exe via `RunAsInvoker`. Pass: the instance starts, `SaveLock: ... opened`. Fail: error 740; the owner decides before 2b ships (D14 would block replaces on that PC).
2. **Guard (A13).** Host in BumperVille; guest opens it. Pass: `Guard: holding tile (4,6) for <host>` within 2 s of the status landing, guest `TileLock: ... peer=1`, both banners. Reverse roles. A peer status for another region or an out-of-bounds tile holds nothing.
3. **Release.** End the guest game: `Guard: released` within 2 s of the reap. Kill the guest launcher: released after 10 minutes (plus or minus 30 s).
4. **Descriptor (A14).** An elevated PowerShell creates the tile mutex with the D14 descriptor; a medium game opens TuggersVille: `peer=1`, no Warning. With a default mutex: Warning `access denied`, city dark.
5. **Name case (A15).** Status `region=` against the folder name on both PCs, mixed-case region: byte-equal, step 2 holds.

**2b:**
6. **Elevation rule.** An elevated SimCity 4 on the guest: a host save is not delivered, banner names SimCity 4; it lands after that game exits. Same with the launcher elevated.
7. **Host save beside a running guest game (A7, A17).** Guest in TuggersVille; host saves DumpsVille and BumperVille. Pass: both `Deliver ... applied` within 5 s / 15 s, equal hashes, new mtimes; the host's neighbor-written TuggersVille waits (`open`) and is dropped when the guest saves it. Dumps of a delivered city equal on both PCs.
8. **Guest push and groups.** Guest TuggersVille saves (neighbor-writing DumpsVille and BumperVille). Pass: one group, `Commit (6,6)` before both record-only members; with TuggersVille open on the host, the whole group waits (`WAITING group`). The host's unchanged copies are never sent back.
9. **Record-only across PCs.** Q20's case through the launchers: guest TuggersVille cancels a deal with closed BumperVille and saves; open both on the host: no deal. Reverse direction. Then revert (a): apart, guest TuggersVille cancels its deal with closed BumperVille and host DumpsVille saves (both neighbor-write BumperVille); Join: host's BumperVille kept on both, and the cancelled deal is back on reload (as documented). Connected, the same within seconds: later arrival wins, deal state follows it.
10. **Revert (b).** Apart, guest TuggersVille cancels the deal, host really saves BumperVille; Join. Pass: `Record City - BumperVille.sc4: dropped, saved here`, host's BumperVille delivered, the deal back on reload.
11. **Pause.** Both save TuggersVille apart; Join. Pass: `Differ (6,6) both` with its group, banners, no city file or snapshot crosses (a host month tick gives no guest `Pull (store change)`), statuses still cross; a guest DumpsVille save made meanwhile waits. Re-sync: confirm text, only (6,6) and its group touched, guest copies in `Conflicts\`, both PCs byte-identical for the group, empty `PAUSED`, then DumpsVille pushes.
12. **Dark save.** Step 2's dark guest city saved: `dark=1`, `Differ (4,6) dark`, pause; Re-sync keeps the host's lit copy. Then a dark host save with a lit guest save: Re-sync asks the host to confirm, the guest's lit copy wins.
13. **Mapping and layout class.** Delete the host's `tiles.txt`, guest saves a city: `WAITING map` until the host enters the region view. A push naming a file mapped to another tile: `REJECTED base`; Re-sync keeps the host's file and moves the guest's name to `Conflicts\` with no game running. Founding on an occupied tile: `base`; on an empty tile with no host game: committed, `foundings` incremented. Rename on either PC: one file per tile, old name archived, only with no game on the receiver.
14. **Copies under the lock (A6).** Host BumperVille saves five times 30 s apart while the host launcher stages its neighbours. Pass: five `ok=1`, no `SaveLock` timeout, every hold under 5 s, staged hashes match. A file over the 5 s hold: the too-large banner, nothing sent.
15. **Disconnect and crash.** A log tail runs `Stop-Process` on the host launcher at the first `FILE` progress line (push and delivery) and at `Commit ... moving`. Pass: no part in Regions, no missing file, the re-push converges (`fh` equal), equal hashes.
16. **Disk and hostile input.** `fsutil file createnew` in `Conflicts\<peer>` to within 1 GiB of the cap: `REJECTED space`, full banner, nothing replaced. S9's unauthenticated cases again; authenticated hostile frames and DBPF indexes in `make test-launcher`.
17. **Failed save (A11).** Hold `City - TuggersVille.sc4` open from PowerShell (`'Read', 'Read'`), Ctrl+S: `ok=0`, hashes unchanged, nothing sent.
18. **Layout file (A12).** Hash `region.ini` before and after a city exit on each PC: equal, no `layout=1`.
19. **Live swap on the build.** Gate step 6 (c) to (e) through the launcher (`Deliver (4,6) retry error=32`, then `applied`).
20. **Stale neighbour record (A18, only if D17 applies).** Gate step 5 on the P4 build, then across PCs. Pass: `Reload (4,6) before save`, the deal and road survive; with Y's own link changed, Y's write wins and is logged.
21. **Latency and soak.** `SaveLock: released` to `COMMITTED` or `APPLIED`, target 15 s, `w32tm`-synced clocks. 60 minutes, a city on each PC, saves every few minutes, region-view round trips, one deal changed each way: every save delivered or committed, no differences, no Warning.

**2c:**
22. **Picker.** Both save TuggersVille apart; Join. Keep guest's: host dialog; No keeps the pause (`REJECTED confirm`); Yes puts the host's group in the host's `Conflicts\`, byte-identical group, empty `PAUSED`. Keep host's ends with `APPLIED resolve=1`. A host save between showing and OK: `REJECTED changed`. A dark row suggests the lit copy.

## 11. Assumptions

| # | Assumption | Basis | Step |
|---|---|---|---|
| A1 | A neighbor write changes only the record, directory and header fields, keeps the size; every save parses | one diff of two pairs (Q20) | Q20 2 |
| A2 | A record-only replace with equal `ch` loads as the game's own neighbor write would | Q20 diff | Q20 3 |
| A3 | The deal state of a pair depends on both files of the group, so a record-only file needs its saver's file | Q20 | Q20 3 |
| A4 | Connections across an edge are carried by the record entry alone | record-connections-641 | Q20 4 |
| A5 | An open neighbour's save does (or does not) write back a stale record of a closed city | untested | Q20 5 |
| A6 | A copy under the save mutex, at most 5 s, never fails a save | Q4 hold times | Q20 6a, Q21 14 |
| A7 | Replacing a tile file not open in a running game is as safe as a local peer's save | Q4, Q15, Q17 | Q20 6c, Q21 7 |
| A8 | A game holds its open city's file with read sharing only, so a move over it fails with error 32 | save-641, S4 | Q20 6b |
| A9 | A city load reads its own file and its neighbours' records (Q20); a file swapped during the load gives the old or the new copy, never a mix or a crash | Q20, Q17 | Q20 6d, 6e |
| A10 | Region-view founding, rename, delete and obliterate do (or do not) take the save mutex | unknown | Q20 7 |
| A11 | A failed save leaves the previous file in place | save-641 step 8 | Q21 17 |
| A12 | `region.ini` is rewritten on city exit with the same bytes | Q4 saw a write | Q21 18 |
| A13 | The plugin treats a launcher-held tile mutex like an instance's | `TileLock::Open` reading | Q21 2 |
| A14 | A medium game opens a D14 mutex made by an elevated process | Windows docs | Q21 4 |
| A15 | `GetDirectoryName()` returns the folder name in on-disk case | code reading | Q21 5 |
| A16 | The GOG exe starts from a non-elevated launcher with `RunAsInvoker` | fix in tree, S10 | Q21 1 |
| A17 | A city saved on one PC loads identically on the other | S10 shared vtables | Q21 7 |
| A18 | `ReloadCity` of a closed neighbour in city view, before a save, keeps that city's deals and connections | Q3 (no deals checked) | Q21 20 |

Verified by code reading: both `.saves` callbacks run inside the save lock scope (`SaveHook641.cpp`), so D5's same-hold read sees `dark` and the save window.

## 12. Supersedes and ground rules

| Where | Becomes |
|---|---|
| PLAN-LAN 2, save mutex used only by the guest around swap renames | used by either launcher by try-lock only, for every copy out of Regions and every replace |
| PLAN-LAN 3, `file.N=name\|size\|hash\|tile` | section 5, `proto` 2 |
| PLAN-LAN 4, "the copy uses no save mutex"; "only the counter means changed here"; journal guest only | D5; D1 and D7; section 7 |
| PLAN-LAN 8, "the host's Regions folder is never written" | section 9 |
| PLAN-LAN 9, writes into Regions only on the guest with no game process | the amended ground rule below |
| PLAN-LAN, Not in slice 1: cross-PC guard; suppression of foreign neighbor writes | 2a (D13); not needed, record-only updates travel (D1) |
| PLAN-LAN, slice 2 hooks: "`archive\` becomes `Conflicts\`" | `archive\` keeps replaced originals; `Conflicts\` keeps losing copies |

**CLAUDE.md ground rule, proposed text** (replaces "The launcher may replace whole files in Regions only on the guest, ..."): "The launcher may replace whole files in Regions on either PC, only from verified staging, with the original archived or copied to `Conflicts\` first, under the save-mutex try-lock, only when no game on that PC has that tile open and neither the launcher nor any SimCity 4 process there runs elevated; adding, removing or renaming a file, or changing `region.ini` or `config.bmp`, also needs no game process on that PC. While the pair is paused it replaces only to apply a resolution the losing PC confirmed. It copies out of Regions only under the same try-lock, reads a `.sc4` only to hash it, and never edits one." The "tile open" clause is provisional until Q20 step 6 passes; if it fails, it becomes "with no game process running on that PC". Windows-free list adds `launcher/Dbpf` and `launcher/SyncRules`. The plugin rule "Never write to `.sc4` save files or the Regions folder" is unchanged (P1, P2 write into the store).

## 13. Tests

`make test`: `tiles.txt`, `dark` and the save window round trip and strict parse. `make test-launcher`: `Dbpf` (section 5, each bound and fallback reason including index size, overlap and duplicate TGI; `ch` stable under record, directory, header and offset changes, changed by any other entry); every new frame and drop rule; journal round trip and the upgrade rule; `SyncRules`: each D1 case, groups (held, paused, resolved together), convergence, host base on disagreement, unexplained mismatch, D3 conditions one at a time, D8 cases and the lasting founding cap, D10 defaults per reason, D16 set behaviour, D13 guard set limits; disk cap and pruning. Windows-only behaviour is covered by Q21.

## 14. Operator steps

1. Add Q20 steps 2 to 7 to `docs/OPEN-QUESTIONS.md`; run them on the current build; record; review this plan with the results.
2. Per increment: build once, deploy the same build to both PCs (the build and `proto` checks refuse mixed builds); back up `Regions\<region>` and the store on both PCs; first connect after the 2b upgrade with SimCity 4 closed on both.
3. On completion of each increment update: `release/README.txt` and `release/SIMTROPOLIS-POST.md` (section 6 text, section 9), `docs/DESIGN.md` (route C, trust), `docs/ARCHITECTURE.md` (tile map, save window, journals, hashes, groups, pause, guard, `Conflicts\`, source map), `docs/PHASES.md` (2a to 2c; the cross-PC guard moves from slice 3 to 2a), `CLAUDE.md` (status, section 12), `docs/OPEN-QUESTIONS.md` (Q21, a Q4 note that both launchers try-lock the save mutex).

## Not in slice 2

- More than one guest or region per connection, launching instances on the other PC, encryption, the Plugins diff list, IPv6, non-ASCII region names.
- Writing any part of a `.sc4` from the launcher or the plugin; a plugin deal ledger or load-time reconciliation (rejected 2026-11-06); validating the record or directory entries.
- Machinery for the D1 reverts; merging two records.
- Guest-side region layout or removals reaching the host (D8, D12).
- Refusing a save in the plugin; an in-game notice for a dark city; the tile picture refresh (Q17, parked).
- The GOG 740 fix: already in the tree (`launcher/Launch.cpp`); its two-PC check is Q21 step 1.
