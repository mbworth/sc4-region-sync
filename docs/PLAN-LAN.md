# Plan: two PCs on one region, slice 1 (live only)

Inputs: `docs/research/lan-two-pcs.md`, `docs/research/lan/save-design.md` (sections 2, 8, 10), `docs/research/lan/audit.md`, `docs/research/lan/smb.md`.

Review round 1 (2026-10-19, security, threading and game safety, simplicity) made these changes. Removed: the heartbeat, `BeatTracker` and the status schema bump. Liveness has one judge (a status file present means Alive) and one 10 minute disconnect clock. The save counter is written before the save, inside the lock, and records the file name, so a city rename is handled. The journal is guest-only, keyed by tile, and the guest hashes its own files. Pairing is CPace over X25519 (Monocypher) in place of PBKDF2, so each code allows one online guess and nothing can be cracked offline. Every handshake field and wire name now has a strict format. The host binds `INADDR_ANY` and the peer address check is the boundary. Host staging moved to the worker without the save mutex. The guest swap refuses while any game process runs and rolls back on failure. Mirroring sends only on change and has caps per peer. The worker shuts down cleanly. Launch stays blocked until the sync is done. The UI lost two dialogs. Loopback is test-only. `WriteFileAtomic` now checks its writes, and the LAN folder's volume is verified.

**Today** every instance shares one disk. **After** two launchers, one per PC, pair once and keep the region live across PCs. On connect the host's region is copied to the guest. Then snapshots and status files are mirrored both ways into each PC's local store, and the plugin's existing publish, pull, store watch and RegionWatch paths do the rest. Guest saves stay on the guest and are flagged "not sent". There are no leases, pushes or picker yet, but the save counters and the journal already carry what slice 2 needs.

## 1. Plugin changes

The plugin gains no network code, no thread and no new game call.

- **P1. Shared parsing.** `regionsync::ParseKeyValues(std::string_view)` comes out of `LoadKeyValues`, and `ParseInstanceStatus(std::string_view, InstanceStatus&)` out of `ReadInstanceStatus`. The launcher's `Status.cpp` keeps its share-delete read and calls these. Schema stays 1. The one new key is the optional `saveCountFailed=1` (P2); unknown keys already parse, so older launchers keep working.
- **P2. Per-tile save counter.** `SaveHook641` gains two callbacks set by the director. `onSaving(path)` runs after `Scope` has taken the lock and before the original `SaveCity`. `onSaved(ok)` runs after the original returns. Both are inside try/catch.
  - In `onSaving` the director writes `<store base>\_saves\<region>\<x>_<z>.saves` through `WriteFileAtomic` with `count+1`. The other keys are `file` (basename of the save path), `prevFile` (the basename the city was loaded from, via `GetCitySaveFilePath` at load, written only when different), `lastSaveUnix`, `simDateNumber`, `population`, `cityName` and the writer triple.
  - `onSaved` rewrites the file with `lastOk`.
  - Every attempt counts, failed ones included, because the file may have changed.
  - On a write failure: Error line, and the sticky `saveCountFailed=1` goes into the status file. The launcher then treats that writer's tile as "not sent".
  - Info line: `SaveCount: (x,z) count=N file='...' ok=1`.
  - The counter lives in the store, so saves made with no launcher running are counted (offline play, study section 10). `_saves` sits beside `_instances`, outside the region store, so the Q17 store watch never sees it.
  - This is the only "changed here" signal.
- **P3. Foreign writer liveness.** `SafeIsAlive` keeps the pid check for our own host. For another host the writer is Alive when `_instances\<pid>-<start>.status` exists and names the same host, pid and start. It is Dead when the file is absent, and Unknown on a read error. Effects:
  - Removal sync works across PCs (`publisherAlive=1`).
  - A gone remote writer's snapshot takes the existing dead-writer path: removed, history dropped, no demolition.
  - Gated by `CheckWriterLiveness` as today.
- **P4. Identity.** `SameHost(a, b)` (case-insensitive) and `SameWriter(a, b)` (`SameHost`, pid, start) in `NeighborSnapshot.h`, used everywhere hosts or writers are compared:
  - `FindTileHolderPid`, `LogConflict` (audit 6);
  - `writer::IsAlive` (audit 21);
  - the launcher (all of section 5).
- **P5. Test-only host alias.** `<UserDir>\SC4RegionSync.local.ini` may set `HostAlias` and `StoreDirectory`. These are read only from there, never from the shipped INI, and are not in the README. `writer::SetHostAlias` runs before `writer::Current()`. `IsAlive` takes the own host from the alias, so local reaping still works under an alias. Used only by loopback (section 7).
- **P6. `WriteFileAtomic`** checks `flush()` and the closed state. On failure it deletes the `.tmp` and returns false, so a full disk never renames a truncated file over a good one (status, snapshots, `.saves`).

`IsReapable` is unchanged: the plugin reaps only its own host. A snapshot or status file from the other PC is removed by the launcher or by the plugin's dead-writer path.

## 2. Launcher topology and threads

The exe has two roles: **Host** (listens) and **Join** (connects). A host serves one guest in slice 1.

| Path | Default | Notes |
|---|---|---|
| `RegionsDirectory` | `Documents\SimCity 4\Regions` | resolved once with `GetFinalPathNameByHandleW` |
| `PluginsDirectory` | `Documents\SimCity 4\Plugins` | DLL version, Plugins hash |
| `StatusDirectory` | existing key | `_instances`. The store base is its parent: region store `<base>\<region>`, counters `<base>\_saves\<region>` |
| LAN folder | `<resolved Regions parent>\RegionSyncLAN\` | `staging\<session>\`, `archive\`, `journal\`, `lan.log`. Host and Join are refused with a banner when its volume serial differs from Regions' |

The **worker thread** (the launcher's first thread) owns everything below:
- sockets, `getaddrinfo`, the frame codec, MAC and CPace;
- hashing;
- copying region files into host staging (section 4);
- hashing the guest's local region files.

It may open files in Regions read-only for those two jobs only. It never writes to Regions, the store, `_instances`, `_saves`, Plugins or `launcher.ini`.

- **Waiting.** The worker blocks only in `WSAWaitForMultipleEvents`, on non-blocking sockets (`WSAEventSelect`), the wake event and the stop event. Hashing and copying check the stop flag per chunk.
- **Queues.** There are two mutex-guarded queues:
  - UI to worker: push, then `SetEvent(wake)`;
  - worker to UI: push, then `PostMessageW(WM_APP_LAN)`.
  The UI drains the whole queue on every `WM_APP_LAN` and on the 250 ms LAN timer, so a lost post costs at most 250 ms.
- **Ownership.** A staging file has one owner at a time and changes owner only through a queue item.
- **Modal dialogs.** While a dialog or message box is up, a `modal` flag makes the LAN timer and the queue handlers return early (items stay queued) and defers `WM_CLOSE`. `WM_APP_LAN` handling sits inside the WndProc try/catch.
- **Shutdown.** `WM_CLOSE` sets stop, signals and joins the worker (3 s), then `DestroyWindow`. If the join times out, the UI saves `launcher.ini` and calls `ExitProcess`.

| Step | Thread |
|---|---|
| Discovery, name resolution, connect and accept, frames, MAC, CPace, key confirmation | worker |
| Host: copy region files to staging (no mutex, section 4), hash, manifest, serve `FILE` | worker |
| Guest: hash local region files, receive and verify `FILE` into staging | worker |
| Plugins hash, DLL version, network profile, volume check | UI, once per Host or Join |
| Guest: process check, archive, swap under the save-mutex try-lock, rollback, journal | UI |
| Change handles (store region dir, `_instances`, `_saves\<region>`) polled on the 250 ms timer; read with `FILE_SHARE_READ \| FILE_SHARE_WRITE \| FILE_SHARE_DELETE`; queue `PUT`/`DELETE` | UI |
| Validate and write incoming files (tmp plus rename), reap, disconnect clock | UI |
| `launcher.ini`, DPAPI, dialogs, rows, banners | UI |

The save mutex `Local\SC4RegionSync.SaveLock.<region>` is used only by the guest's UI thread, only around the swap renames:
- opened with `CreateMutexW`;
- taken only with `WaitForSingleObject(m, 0)`, retried on a 100 ms timer;
- held for milliseconds.

The launcher's open makes the plugin log `SaveLock: opened ... peer=1` with a single instance. `lan.log` records the launcher's own open, and Q4 gets a note.

## 3. Protocol

### Ports and discovery

- TCP `Port` (default 47641) and UDP `Port+1`, both bound to `INADDR_ANY`. IPv4 only, no UPnP.
- The boundary is the peer address check: at `accept` and on every UDP datagram, the peer must be in 10/8, 172.16/12 or 192.168/16, or be 127.0.0.1 when `AllowLoopback=1`.
- Why `INADDR_ANY`: per-address binds break whenever DHCP, a VPN or a Wi-Fi roam changes the adapters. A public NIC with a manual port forward is still refused at `accept`.
- Guest `Join` sends `DISCOVER {magic=SC4RS, proto}` to 255.255.255.255 every 1 s while the dialog is open (unicast to 127.0.0.1 with `AllowLoopback`).
- The host replies `{proto, site, siteId, build, port}`.
- A typed name or address is resolved on the worker (`getaddrinfo`, `AF_INET`). It must be private, and gets the same `DISCOVER` by unicast.
- Discovery is unauthenticated and only fills the list. The list shows an ID (the first 8 hex of `siteId`). It is an identifier, not a security check.
- `PeerAddress` is updated only after an authenticated handshake, so a spoofed reply cannot redirect the reconnect loop.

### Frames

```
u32 len        bytes after this field; <= 4 KiB before the handshake completes, <= 1 MiB + 64 after
u8  version    1
u8  type
u64 counter
u32 textLen    <= 64 KiB; UTF-8 key=value lines, same syntax as snapshots
... text
... data       len - 14 - textLen - 32 bytes, raw file bytes (PUT, FILE)
u8[32] mac     keyed BLAKE2b-256(key, dir || version..data), dir = 'G' or 'H'
```

- Counters start at 1 in each direction after the handshake, and each frame must carry exactly previous+1.
- After the handshake, any of these drops the connection at once with no reply, logged with the peer address:
  - a MAC failure;
  - a bad counter or length;
  - a field over its cap;
  - an unknown type.
- Half-open connections: at most 4, and 1 per address.

### Handshake and pairing

**Field formats** are checked at parse; anything else is refused:

| Field | Format |
|---|---|
| `siteId` | 32 lowercase hex, random per install |
| `site` | 1 to 15 of `[A-Za-z0-9-]` |
| `build` | digits, dots, `+`, 7 hex |
| `pluginsHash` | 64 hex |
| `nonce` | 32 hex (16 bytes from `BCryptGenRandom`) |
| `Y` | 64 hex |
| region names | printable ASCII, the section 4 name rules |

**Pairing window.** The host accepts a new guest only while hosting with no paired guest, or after "New code".
- The code is 6 digits from `BCryptGenRandom`, shown in the host's status strip.
- It is burned after one attempt, failed or not, or after 10 minutes.
- At most one pairing attempt per 2 s. The strip shows the address of each burned attempt.

**Pairing (CPace).**
1. G to H `HELLO {pair=1, nonceG, ...}`. This is the only frame without a MAC; the host's only action on it is a reply.
2. H to G `HELLO {nonceH, Y_H, ...}`.
3. G to H `PAKE {Y_G, confG}`.
4. H to G `CONFIRM {confH}`.

Key derivation:
- Generator `P = crypto_elligator_map(BLAKE2b("sc4rs-cpace" || code || hostSiteId || guestSiteId || nonceG || nonceH))`.
- Each side picks a random scalar `s`, sends `Y = crypto_x25519(s, P)` and computes `Z = crypto_x25519(s, Y_peer)`.
- `K = BLAKE2b(Z || transcript)`, where the transcript is both hellos plus `Y_G` and `Y_H`.
- `confG = MAC(K, "G" || transcript)`. The host checks it before it reveals `confH`.
- A wrong code fails `confG`: the code burns, and the reply is `refused reason=code`.

Each run allows one online guess, and a captured exchange cannot be tested offline. After `CONFIRM` both sides store `K` as `PeerKey`, and `PeerSite` and `PeerSiteId` are fixed.

**Paired reconnect.**
1. G to H `HELLO` (MAC with `K`).
2. H to G `HELLO` (MAC with `K` over both hellos).
3. G to H `CONFIRM` (MAC with `K` over both hellos plus `nonceH`).

These rules apply:
- An unknown `siteId`, or a `HELLO` that fails its MAC, gets `refused reason=unpaired`. The only effect of that unauthenticated refusal is a re-pair prompt.
- After 3 handshake refusals in a row the guest stops and shows "pairing lost, Join again".
- A later `HELLO` whose `site` differs from the stored `PeerSite` is refused.
- A peer whose `site` is `SameHost` with ours is refused.

Session key `Ks = BLAKE2b(K, "session" || nonceG || nonceH)`. A half-open old connection from the same `siteId` is replaced only after the new `CONFIRM` verifies.

**Versions.** `build` (from `version.h`: version plus the git short hash, baked in by the Makefile) must be equal. Locally, each launcher also requires the file version of `<PluginsDirectory>\SC4RegionSync.dll` (`GetFileVersionInfoW`) to equal its own. If either check fails, the result is `refused reason=version` with both values in the message. If `pluginsHash` differs, the result is a banner only. `pluginsHash` is BLAKE2b over the sorted `relpath-lowercase|size` lines for every file under Plugins except `RegionSync\`, `SC4RegionSync.*` and `*.log`.

### Messages

| Type | Direction | Fields | Receiver |
|---|---|---|---|
| `HELLO` | both | `proto`, `role`, `site`, `siteId`, `nonce`, `pair`, `build`, `pluginsHash`. Host adds `result`, `reason`, `region.N`, `Y` (pairing) | as above |
| `PAKE` | G to H | `Y`, `conf` | pairing only |
| `CONFIRM` | both | `conf` (H to G when pairing), MAC (G to H when paired) | session starts |
| `SELECT` | G to H | `region` (ASCII, else refused with a message) | host stages it and sends `MANIFEST`. One per connection plus 3 retries |
| `MANIFEST` | H to G | `region`, `count`, `file.N=name\|size\|hash\|tile` (`tile` is optional and comes from the host's `_saves` `file`) | guest refuses an empty manifest, otherwise diffs (section 4) |
| `GET` | G to H | `region`, `name` (must be in the manifest) | host streams `FILE` from staging |
| `FILE` | H to G | `name`, `offset`, `last`; data up to 256 KiB | `offset` must equal the bytes received and `offset+len` must be <= the manifest size. Hash checked on `last`; a mismatch discards the file and sends `SELECT` again |
| `SYNCED` | G to H | `region`, `downloaded`, `kept`, `notSent` | host shows "synced" and deletes the session staging |
| `INDEX` | both | `kind=snapshot\|status`, `region`, `name.N` | receiver deletes its copies from this peer not listed |
| `PUT` | both | `kind`, `region` (snapshot), `name`; data = file bytes | section 5 |
| `DELETE` | both | `kind`, `region`, `name` | delete only if the local file's writer is `SameHost` with the peer |
| `PING` | both | none | keepalive |
| `BYE` | both | `reason` | close; the guest does not reconnect for `unpaired`, `version` or `cap` |

Mirroring starts after `SYNCED`, or right after the handshake when the guest postpones the swap. Each side sends `INDEX`, then a `PUT` for each own file. Status files go before snapshots on start. On exit a snapshot `DELETE` goes before its status `DELETE`, so a reader never sees a snapshot whose writer has no status file (P3 reads that as Dead).

### Timeouts and reconnect

| What | Value |
|---|---|
| TCP connect | 5 s (non-blocking) |
| Handshake complete | 10 s |
| `PING` when nothing else was sent | every 5 s |
| Connection dead | 20 s with no valid frame |
| Guest reconnect | 1, 2, 4, 8, 16, then every 30 s, plus or minus 20 % jitter. Resets only after `SYNCED` or 60 s connected |
| Host | one guest; any other gets `refused reason=busy` |
| Peer files after disconnect | kept 10 minutes. Then the launcher deletes the peer's status files, then that peer's snapshots |
| Swap try-lock | 100 ms retries until it holds the lock or a game process starts (then postponed) |

## 4. First sync and journal

On every connect the host copies to the guest the region the guest selects. The last selected region is remembered.

1. **Host, worker:**
   - Check free space first: region size plus 10 % plus 256 MiB.
   - For each file in `Regions\<region>\` (flat), copy to `staging\<session>\`. Subfolders are logged and skipped.
   - The copy uses no save mutex. Open with `GENERIC_READ`, `FILE_SHARE_READ | FILE_SHARE_DELETE`, and record the file ID (`GetFileInformationByHandle`), size and mtime. Copy, hash with BLAKE2b, then open the path again and compare. Any difference means a save or neighbor write happened in between, so the file is retried (5 times, then the sync fails with a message).
   - A staged file from an earlier session whose source ID, size and mtime are unchanged is reused.
   - Then the host sends `MANIFEST`.
2. **Guest, worker:**
   - Validate every name (below).
   - Check the caps: 1 GiB per file, 1024 files, not empty.
   - Hash the local region files.
   - Pass the manifest and the local hashes to the UI.
3. **Guest, UI: diff by tile.** It is game-free and unit-tested. The inputs are the guest's `_saves` (`count`, `file`, `prevFile`), the journal and the manifest.
   - **Kept tile.** `count > baseCount`, or `saveCountFailed=1` for that writer's tile. The tile's current local file is kept and flagged "not sent". No manifest file that belongs to the tile is ever downloaded. A file belongs to a tile when it is named by the journal's `file`, the `.saves` `file` or `prevFile`, or the manifest's `tile`.
   - **Any other file** (the other tiles, `region.ini`, `config.bmp`): hash equal is the same file, hash different is downloaded. This includes the guest's own neighbor writes into host tiles: only the counter means "changed here" (smb.md).
   - **Local file not in the manifest:** kept if it belongs to a kept tile, otherwise archived.
   - **Free space:** checked against the downloads plus the archive moves.
4. **Guest, worker:** `GET` each download into `staging\<session>\` and verify it.
5. **Guest, UI: swap.**
   - Only when no SC4 game process runs on the guest at all. Otherwise the strip says "close SimCity 4 on this PC to finish the sync"; live mirroring runs anyway.
   - Launch is disabled until `SYNCED` with no pending swap.
   - Under the try-lock: re-check the processes. For each file, first move the original to `archive\<yyyy-mm-dd hhmmss>\`, then `MoveFileExW(staging, target, REPLACE_EXISTING | WRITE_THROUGH)`. Move extras to the archive.
   - On any failure every swapped original is moved back, the staging copy stays, and the error is reported.
   - Unlock, write the journal, send `SYNCED`.
6. **No folder, or a folder the journal does not know:**
   - The folder is created with the host's exact name and case.
   - An existing unknown folder (compared case-insensitively) is moved to `archive\` after a Yes/No, then a full download follows.
   - Region names are compared case-insensitively in the launcher.

**Journal** (guest only): `journal\<peerSiteId>\<region>.txt`, key=value, one line per tile `x_z`: `file`, `baseHash`, `baseCount`. After a swap, every tile with a local `.saves` gets `baseCount = count`, and `file` and `baseHash` are set from the swapped file. The UI refreshes the counters when the `_saves\<region>` change handle fires and at start.

**Name rules** (`Manifest.cpp`, unit-tested):
- **Region names:** printable ASCII, 1 to 120 characters, none of `<>:"/\|?*~` or control characters, no leading or trailing space or dot, not `.` or `..`, no reserved device name with any extension.
- **Region files:** the same rules, and only `*.sc4`, `region.ini` and `config.bmp`. Compared case-insensitively. A manifest with case-only duplicates is refused.
- **Snapshots:** `^(0|[1-9][0-9]{0,3})_(0|[1-9][0-9]{0,3})\.snapshot$`, with the tile inside the region's bounds (`config.bmp` size).
- **Status files:** `^[1-9][0-9]{0,9}-[1-9][0-9]{0,19}\.status$`.
- **Targets** are built only in these forms:
  - `<RegionsDirectory>\<region>\<name>`;
  - `<store base>\<region>\<name>`;
  - `<StatusDirectory>\<name>`;
  - `<LAN folder>\journal\<peerSiteId>\<region>.txt`.
  Each target's `GetFullPathNameW` parent must equal its directory. A region folder that is a reparse point is refused.
- **Staging names** (`fNNNN.part`) are chosen by the worker; no path is built from a wire string.

## 5. Mirroring rules (UI thread)

- **Outbound.**
  - On a change handle signal, read each `*.snapshot` and `*.status` with full sharing.
  - Forward a `PUT` only when the file's writer is `SameHost` with us, and only when its bytes' hash differs from the last one sent under that name.
  - A file that disappears is forwarded as `DELETE` only if the launcher's index says its writer was ours.
  - Files written by the peer signal the handles too, but their writer is not ours, so nothing echoes back.
- **Inbound `PUT`.**
  - Caps: snapshot 256 KiB, status 16 KiB. Per peer: at most `MaxInstances` status files and one snapshot per tile of the selected region. Past a cap the launcher sends `BYE reason=cap` and logs it.
  - Reject the file unless its writer host is the peer's `site` (`SameHost`) and not ours.
  - A snapshot's `regionX` and `regionZ` must match its canonical name. A status file's `pid` and `start` must match its name.
  - Skip the write when the bytes equal the local file.
  - **Refuse a snapshot for tile (x,z) when either holds:** the local snapshot for that tile has a live writer on our host (pid check), or a local status file (our host) shows that tile in `city` state.
  - On refusal, show "tile (x,z) is open on both PCs" and keep the latest bytes in one pending slot per tile, retried on the 250 ms timer.
  - Otherwise write `<name>.tmp` and rename over (`WriteFileAtomic` rules, P6). Then check again: if a live local writer appeared, restore the local bytes.
- **Inbound `DELETE`:** only when the local file's writer is `SameHost` with the peer.
- **Reaping.**
  - The launcher reaps its own host's dead pids as today (`Discover`), and the mirror forwards those deletes.
  - On disconnect the 10 minute clock starts. When it ends, the peer's status files are deleted, then the peer's snapshots.
  - In between, P3 still reads remote writers as Alive, so a short drop or a launcher restart keeps mirror history. A reconnect inside the 10 minutes stops the clock, and `INDEX` reconciles.

## 6. Settings and UI

`launcher.ini` gets a new `[Lan]` section:
- `RegionsDirectory`, `PluginsDirectory`, `Port=47641`.
- `SiteId` (generated once).
- `Role` (`Host`, `Join` or empty; resumed at start).
- `PeerSite`, `PeerSiteId`, `PeerAddress`, `Region`.
- `PeerKey`: 32 bytes protected with `CryptProtectData` (current user), stored as base64.
- Test-only and undocumented: `AllowLoopback=0`, `HostAlias=`.

It holds one peer record; pairing again replaces it. Every value written there is generated locally or has passed the field formats in section 3, so a CR/LF can never reach `WritePrivateProfileStringW`. The code is never stored.

- **Toolbar:**
  - `Host`: check button, listen or stop.
  - `Join`: a dialog with the found hosts (name, address, ID, paired), an address box, a 6-digit code box (enabled for an unpaired host) and a region combo, filled after the handshake.
  - `Re-sync`: guest only, connected, no game process. After a Yes/No naming the tiles, it archives the not-sent tiles, runs a full sync and resets the journal.
- **Status strip:** a second part left of the resource line.
  - Host: `Hosting, code 482 913 (New code)`, with the address of the last burned attempt. Once paired: `Hosting, paired with GUEST-PC, synced 14:00:01, last mirror 14:03:12`.
  - Guest: `HOST-PC: syncing 3/14`, `synced, 2 tiles not sent`, `reconnecting in 8 s`.
  - The tooltip lists the not-sent tiles and the peer's `build`.
- **Banner:** one line above the list showing the most important of these. The tooltip lists all active ones.
  - refused (with reason);
  - Public network profile on the host (`INetworkListManager`);
  - Plugins differ;
  - sync pending (game running on the guest);
  - tile open on both PCs;
  - tiles not sent.

  In loopback the launcher also warns when a local status file's host is not `SameHost` with its own `HostAlias`.
- **Rows:**
  - A `Host` column; rows are keyed by the triple.
  - Remote rows are read-only: no raise, List only, no End Task.
  - Remote rows are excluded from the resource sample, from `liveUserDirs` and from pending-launch matching (audit 22, 23).
  - The orphan scan skips a pid named by any status file whose `start` equals that local process's creation time.

## 7. Loopback mode (test-only, one Windows box)

`scripts/Setup-MultiInstance.ps1 -LoopbackGuest 2`:
- Asserts `(Get-Item Instance2\Regions).LinkType -eq 'Junction'`.
- Removes the junction non-recursively (`[IO.Directory]::Delete($path)`) and creates an empty real folder.
- Writes `Instance2\SC4RegionSync.local.ini` with `HostAlias=<PC>-GUEST` and `StoreDirectory=<Instance2>\RegionSyncStore`.

`-RestoreLoopback` moves the guest folder to the LAN `archive\` (never deletes it) and recreates the junction. Plugins stays a junction, so the build and Plugins checks match by construction.

- **Host launcher:** the normal install, with `AllowLoopback=1`.
- **Guest launcher:** a copy in `SC4RegionSyncLauncher-Guest\` whose `[Lan]` has `HostAlias=<PC>-GUEST`, `RegionsDirectory=<Instance2>\Regions` and `AllowLoopback=1`. Its `StatusDirectory` is `<Instance2>\RegionSyncStore\_instances`. It joins 127.0.0.1.
- **Instances** start from the `SC4Instance1` and `SC4Instance2` tasks.
- **Limits:**
  - Both sides share a session, so they share the save and tile mutexes.
  - The guest swap's "no game process" rule also sees the host's instance, so the sync happens before either instance starts.
  - Firewall behaviour on the guest and the cross-PC same-tile case need two PCs.

## 8. Security

- **An attacker on the LAN can:**
  - read discovery replies and everything in transit (frames are signed, not encrypted): region names, region files, snapshots, status files;
  - reset connections (the guest reconnects);
  - make one online guess per displayed code, which burns it. Repeated attempts can keep pairing from succeeding, so the strip shows each attempt's address.
- **It cannot:**
  - test codes offline (CPace);
  - reach the launcher from outside the LAN (peer address check, no UPnP; a manual port forward is still refused at `accept`);
  - send anything that is acted on without `K` (the pairing `HELLO` only triggers a reply);
  - replay frames (fresh nonces in every session key, exact counters);
  - write outside the region folder, the store, `_instances` or the journal (name rules, fixed targets, staging names chosen locally);
  - write a file that claims our host, or delete a local writer's file;
  - write over a tile that is open here;
  - impersonate the paired host or guest.
- **Pairing trusts that PC with your region and your game.**
  - A paired peer's snapshots drive this PC's mirror: edge pieces (a failed lay demolishes cells), connection and deal removals, deal amounts. All of it lands in this PC's `.sc4` on its next save. Slice 1's "the host's Regions folder is never written" is true of the launcher only.
  - A paired host hands the guest `.sc4` files for the 2003 game's loader. Assume a malicious one can run code on the guest.
  - An empty manifest is refused. A short manifest archives the guest's other files, never deletes them.
  - Before implementation, confirm that the snapshot parser's bounds (`kMaxListEntries`, deal amount and expiry ranges, connection cells) are tight enough for an untrusted source.
- **README** says this in plain words: pair only with your own PC on a network you trust, and confidentiality is not provided.

## 9. What must not break

- Without Host or Join nothing listens and nothing is sent: no socket, no firewall prompt. Single-PC play is as today, apart from the `_saves` files and the `.saves` write inside each save.
- The plugin has no network code, no thread and no new game call. The save callbacks are in try/catch and never prevent the original `SaveCity`.
- The launcher writes into Regions only on the guest, and only under all of these conditions:
  - whole files from verified staging;
  - originals archived first, with rollback;
  - with the save mutex try-locked;
  - only when no game process runs.
  It never writes to the host's Regions. It never takes the save mutex anywhere else, so it can never stall a game save.
- The Q4 save lock, Q14 tile guard, and Q17 store watch and RegionWatch are unchanged. A launcher-written snapshot looks to them like a local peer's. Snapshots are rewritten only when their bytes change, so Q17 pulls only on real publishes.
- `NeighborSnapshot`, `InstanceStatus`, `Frame`, `Manifest` and `Journal` stay game-free and Windows-free.
- `make test` and `make test-launcher` pass, and `make launcher` builds with zero warnings.
- `make deploy` still refuses while the game or a launcher runs.

## 10. Experiment: Q18 slice 1 (steps S1 to S10)

Region New York: BumperVille (4,6) and TuggersVille (6,6). Back up the region folder first. Hashes: `gci <region> -Filter *.sc4 | % { '{0} {1}' -f $_.Name, (Get-FileHash $_.FullName).Hash.Substring(0,12) }`.

1. **S1 Setup and boundary.** Run `-LoopbackGuest 2` and start both launchers.
   - At the first `Host`, record the firewall prompt exactly.
   - From a non-private address, or by temporarily removing 127.0.0.1 from the allowed set: `refused at accept` in `lan.log`, nothing else.
   - Fail: a connection served from a non-private peer.
2. **S2 Discovery and pairing.**
   - Join lists the host within 2 s. If not, typed 127.0.0.1 works; record which.
   - Wrong code: guest "wrong code", host shows a new code and the attempt's address; a second try within 2 s is refused.
   - Right code: both show paired. `launcher.ini` has `PeerKey`, not the code.
   - Restart both: they reconnect without a code.
   - Re-pair: the host clears its peer and pairs a second guest copy. The old guest gets `unpaired` and after 3 refusals shows "pairing lost, Join again".
   - Fail: pairing with a wrong code, the code on disk, or an endless reconnect loop.
3. **S3 Handshake.**
   - A guest built from another commit is refused with both `build` values, and does not loop.
   - Two PCs: an extra file in the guest's Plugins gives the banner only.
4. **S4 First sync.**
   - The guest region is absent: full download, equal hashes on both sides.
   - Any SC4 process running on the guest: "close SimCity 4", nothing replaced, Launch disabled until `SYNCED`.
   - An existing unknown `New York` is asked about and moved to `archive\`.
   - Rollback: hold one target open with PowerShell (`[IO.File]::Open(..., 'Open', 'Read', 'Read')`) during the swap. Every original is restored and the error is shown.
5. **S5 Live mirroring.**
   - Instance1 in BumperVille (host), Instance2 in TuggersVille (guest).
   - Each month tick on one side gives `Pull (store change)` on the other within a sim day. No pull without a publish.
   - Remote rows are read-only, with the Host column.
   - Record the latency from `Publish (month tick)` to the other side's `Pull (store change)`.
6. **S6 Gone writer and disconnect.**
   - End the guest game from Task Manager. The host row goes within 2 s, and the host plugin logs `writer pid N on host <PC>-GUEST is gone, removed` at its next pull, with no demolition.
   - Restart the guest game and stop the host launcher mid-play. The guest shows the banner and backs off 1, 2, 4, 8 s.
   - Restart the host launcher within 10 minutes: `INDEX`, pulls resume, history kept (no `history dropped`).
   - Stop it again for over 10 minutes: the peer status files, then the snapshots, are removed; the plugin logs the dead-writer path, with no demolition.
7. **S7 Removal across PCs.**
   - The guest bulldozes a road crossing the shared edge.
   - Pass: the host mirror removes it (`publisherAlive=1`).
8. **S8 Guest saves stay local.**
   - Guest Ctrl+S gives `SaveCount: (6,6) count=1 file='City - TuggersVille.sc4' ok=1`, and the launcher shows "1 tile not sent". The host hash is unchanged.
   - Exit the guest city and reconnect. TuggersVille is kept. BumperVille, rewritten by the guest's neighbor write (no counter), is downloaded again.
   - **Offline:** close the guest launcher, open TuggersVille, save, exit, start the launcher, Join. The tile is kept and flagged, and the host hash is unchanged.
   - **Rename:** rename TuggersVille on the guest and save. `.saves` has `file` and `prevFile`, the sync downloads neither name, and the folder holds one city for (6,6).
   - `Re-sync`: the not-sent file goes to `archive\` and is replaced, and the flag clears.
9. **S9 Hostile input.**
   - A PowerShell `TcpClient` sends 100 random bytes, then a frame of more than 4 KiB before the handshake, then a well-formed frame with a bad MAC.
   - The host drops each one and logs it, and no file is written.
   - A `HELLO` whose `site` contains CR/LF is refused, and `launcher.ini` is unchanged.
10. **S10 Two PCs.**
    - One PC hosts and a second PC joins, wired, then on Wi-Fi.
    - Firewall: one prompt on the host, none on the guest.
    - With the host's profile Public: the banner shows, and the guest's connect fails with a clear message.
    - Repeat S2, S4, S5 and S6.
    - Both PCs open BumperVille: "open on both PCs" on both launchers, neither store overwritten. Exit one and the other's snapshot lands.
    - Latency from a publish to the other PC's `Pull (store change)`, with `w32tm` synced clocks.
    - 30 minutes in city on both: no Warning lines, and every peer month tick is pulled within a sim day.

The study's Q18 steps 6, 7 and 14 are covered by S1, S3, S4 and S10. Its steps 1 to 5 and 8 to 13 stay for slices 2 and 3.

## 11. Docs and build to amend

- `docs/OPEN-QUESTIONS.md`: `## Q18` with the study's steps and S1 to S10, empty Observed slots. Q4 gets a note: the launcher's mutex open shows as `peer=1`.
- `docs/PHASES.md`: split the two-PC item into slices 1, 2 and 3.
- `docs/DESIGN.md`: store per PC, launcher channel, host-authoritative region at rest.
- `docs/ARCHITECTURE.md`:
  - two-PC topology;
  - the worker thread and its queues;
  - lifecycle rows for the save callbacks and `_saves`;
  - store layout;
  - source map.
- `docs/research/lan/save-design.md`: note what this plan changed. No Reclaim. Archive outside Regions. CPace pairing. No heartbeat. `SaveHook641` had no `onExit`. The counter is in `_saves`, not the status file.
- `README.md`: Host and Join, firewall, pairing, the security paragraph, "guest saves are not sent in slice 1". Nothing about `HostAlias` or loopback.
- `CLAUDE.md`:
  - status line;
  - ground rule: "the launcher may replace whole files in Regions only on the guest, from verified staging, originals archived first, under the save-mutex try-lock, with no game process running";
  - `vendor/monocypher` is a plain copy, not a submodule;
  - the Windows-free file list and `make test-launcher`.
- `vendor/monocypher/`: `monocypher.{c,h}` plus `LICENCE` (CC0 or 2-clause BSD), one pinned release, as a plain copy.
- `src/version.h`: `0.2.0`. The Makefile passes `BUILD_ID` (`0.2.0+<git short hash>`) to the launcher.
- Plugin sources:
  - `src/SyncSettings.{h,cpp}`: the `<UserDir>\SC4RegionSync.local.ini` read;
  - `src/NeighborSnapshot.{h,cpp}`: `ParseKeyValues`, `SameHost`, `SameWriter`, the P6 `WriteFileAtomic` checks;
  - `src/InstanceStatus.{h,cpp}`: `ParseInstanceStatus`, `saveCountFailed`;
  - `src/WriterIdentity.{h,cpp}`: `SetHostAlias`, case-insensitive `IsAlive`;
  - `src/SaveHook641.{h,cpp}`: the two callbacks;
  - `src/RegionSyncDllDirector.cpp`: P2, P3, P4.
- `launcher/`:
  - Windows-free: `Frame.{h,cpp}`, `Manifest.{h,cpp}` (names, diff), `Journal.{h,cpp}`, `Handshake.{h,cpp}` (field formats, CPace transcript and confirmation on Monocypher);
  - `Lan.h`;
  - `Net.cpp`: worker, sockets, staging copy, hashing;
  - `LanCrypto.cpp`: `BCryptGenRandom`, DPAPI;
  - `Lan.cpp`: UI-thread controller, mirror, swap, reaping;
  - `LanUi.cpp`: Join dialog, strip, banner;
  - `Status.cpp` uses `ParseInstanceStatus`;
  - `main.cpp`: Host column, triple keys, LAN timer, `WM_APP_LAN`, `modal` flag, Launch gate;
  - `launcher.rc` and `resource.h`: `IDD_JOIN`.
- `Makefile`:
  - `LAUNCHER_SRCS` adds the new files, `src/InstanceStatus.cpp`, `src/NeighborSnapshot.cpp` and `vendor/monocypher/monocypher.c`.
  - `LAUNCHER_LIBS` adds `ws2_32 bcrypt crypt32 version ole32`.
  - `test` adds the P1, P3 and P4 cases.
  - New `test-launcher` target: `gcc -c` on Monocypher, then `g++ -std=c++20 -Wall -I src -I launcher -I vendor/monocypher tests/launcher_lan.cpp launcher/Frame.cpp launcher/Manifest.cpp launcher/Journal.cpp launcher/Handshake.cpp src/InstanceStatus.cpp src/NeighborSnapshot.cpp monocypher.o`. It covers:
    - frames: real MAC round trip, MAC failure, counter regression and skip, length caps before and after the handshake;
    - CPace: right code gives equal keys and verifying confirmations, wrong code fails, transcripts bound;
    - field formats, including CR/LF;
    - name rules: traversal, device names, `~`, case duplicates, non-canonical tiles, extension allow-list, ASCII region names;
    - the manifest diff: every case in section 4, including the rename, the offline save and `saveCountFailed`;
    - `ParseInstanceStatus`;
    - journal round trip.
  - `logs` also pulls `RegionSyncLAN\lan.log`.
  - `deploy` also copies the exe to `SC4RegionSyncLauncher-Guest\` when that folder exists.
- `scripts/Setup-MultiInstance.ps1`: `-LoopbackGuest N`, `-RestoreLoopback` (section 7).

## Not in slice 1

- Leases, pushes, commits, generations, conflict copies.
- Pulling host saves while connected (only on connect).
- The reconnect picker (study section 10). Slice 1 keeps and flags guest-saved tiles.
- A cross-PC same-tile guard. Slice 1 has a banner and refuses the write.
- `_lan\leases`, `launcher.alive`, the plugin's launcher-missing notification.
- Counters in the manifest, the host-side journal, status save keys.
- Suppression of foreign neighbor writes (study 2.9).
- Plugins diff list, more than one guest, more than one region per connection, IPv6, encryption.
- The `cityLoadedAt` clock-skew fix (audit 17; Windows time sync assumed). Non-ASCII region names.
- Launching instances on the other PC.
- A host hash cache beyond reusing unchanged staged files.

## Slice 2 hooks already in place

- `_saves` counters (offline saves included) with `file` and `prevFile`, and the guest journal's `baseHash` and `baseCount`. The study's section 10 divergence test is `count > baseCount` on each side, once the host keeps its own journal.
- The `_saves\<region>` change handle: the guest's push trigger.
- Triple identity and `SameHost` everywhere.
- Staging, verify, archive-first swap with rollback: the same path serves host-change pulls on the guest and commits on the host.
- `proto` and the message type space for `LEASE`, `PUSH`, `COMMIT`. `archive\` becomes `Conflicts\`.
