## Session log: files and the lines that explain a session

What the log directory (`%APPDATA%\eglantine\log` on Windows) holds, and the lines the emulator writes so that a bug report can be read from the log alone. The lines below are the exact text, with example values. Nothing here changes what the emulator does; it only writes down what it did.

## Log files

| File | What it is |
|---|---|
| `eden_log.txt` | The current session. |
| `eden_log.<YYYYMMDD-HHMMSS>.txt` | The previous session, plain text. The stamp is the local time of its last write. If two logs end in the same second the second one is `eden_log.<stamp>-1.txt`. |
| `eden_log.<YYYYMMDD-HHMMSS>.txt.zst` | An older session, compressed with zstd (`zstd -d`, or any zstd-aware viewer). |
| `eden_log_2.txt` | Used instead of `eden_log.txt` when the current log cannot be moved (it is open in another emulator process). Its archives are named `eden_log_2.<stamp>.txt`. |

- At launch the previous `eden_log.txt` is renamed, and the newest 5 archives of a log are kept; older ones are deleted. An empty log is simply overwritten.
- Only the newest archive stays plain. The others are compressed once, in the background at low priority, a few seconds after launch. A compression that is cut short by closing the emulator leaves the plain file and a `.zst.part` that the next launch deletes.
- An archive of the previous scheme, `eden_log.txt.old.txt`, is renamed into the new scheme once.
- The Android build still keeps one `eden_log.txt.old.txt`: its share-log button reads that name.
- `tools/nx2/incident.py` and `session_watch.py` read only the current log. `tools/nx2/debug-session.sh` keeps the current log and the newest archive as `prev-*`.
- The gpu log (`eden_gpu.log`) has its own rotation and is not part of this.

Lines written at launch, in this order, once the log is open:

```
Log retention: previous session log kept as eden_log.20260518-221503.txt
Log retention: the old .old.txt log was renamed to eden_log.20260517-193012.txt
Log retention: kept the newest 5 session logs, deleted eden_log.20260510-120000.txt.zst
Log retention: compressed 2 older session log(s) to .zst
```

The first three appear only when they happened; the last comes from the compression thread.

## Which game file ran

One line when the title is published (the same point that logs `Nx2Cache identity`), for a command-line launch and for the Qt front end:

```
Game file: title=0100E95004038000 version=65536 kind=nup path="D:\Games\XC2\xc2.nup" size=14.20 GiB (15245867008 bytes) modified=2026-05-18 21:14:03 addons=2 (1 update, 1 dlc, 0 mod: update "Update" v1.1.0, dlc "Expansion Pass" v0) content=4f3a9c0d12ab34ef
```

- `kind` is the file extension, or `directory` for a loose tree. A missing file has `exists=no` instead of size and time. `modified` is local time.
- `addons` counts the enabled add-ons the host identity counted (update, DLC, mod); at most 8 are listed.
- `content` is the cache content stamp of the `Nx2Cache identity` line.

## NUP integrity check

When the game file is a `.nup`, a background thread hashes every member with SHA-256 and compares it with the hash stored in the container. It never delays the boot.

```
NUP integrity check started in the background: xc2.nup, 1234 member(s), 14.20 GiB, packages: 1 base, 1 update, 3 addon; reads at most 32 MiB/s at low priority and pauses while the game is reading
NUP integrity check passed: xc2.nup, 1234 member(s), 9.87 GiB read, 0 corrupt, took 412 s (95 s paused while the game was reading)
```

With a corrupt member the second line says `FAILED` and a Warning follows that names every corrupt member, 12 to a line, with what was wrong (`SHA-256 mismatch`, `zstd data corrupt`, `short read of the container`, ...):

```
NUP integrity check FAILED: xc2.nup, 1234 member(s), 9.87 GiB read, 19 corrupt, took 412 s (95 s paused while the game was reading)
NUP integrity: xc2.nup has 19 corrupt member(s), 1-12: base/romfs/bdat/a.bdat (SHA-256 mismatch), base/romfs/b.wismt (zstd data corrupt), ...
NUP integrity: xc2.nup has 19 corrupt member(s), 13-19: base/romfs/m.bin (SHA-256 mismatch), ...
```

Other outcomes:

```
NUP integrity check skipped: xc2.nup is unchanged (size, modification time) since it was checked on 2026-05-18 21:20:41: 1234 member(s), 0 corrupt
NUP integrity check cancelled after 311 of 1234 member(s) (0 corrupt so far); it runs again at the next launch
NUP integrity check could not read the container of xc2.nup: <loader status>
```

An unchanged file is not read again: the result is kept per file (path, size, modification time) in `nup_integrity.txt` in the cache directory, 16 files at most. A file that failed has its Warning repeated from that cache at every launch, until the file changes. A cancelled check is not kept.

How it keeps out of the way:

- Own thread at background priority (on Windows also background IO and memory priority and the efficiency cores), own file handle, no use of the game's file layers, so the game's read statistics do not include it.
- At most 32 MiB/s of container reads, in 1 MiB steps. The memory used is a few MiB: zstd members are decompressed as a stream and never enter the decompress cache.
- It pauses (checks every 250 ms) while the game reads 200 or more times a second or 8 MiB/s or more.
- Closing the game cancels it within one step; the shutdown waits for it.
- At the 32 MiB/s limit one core is busy for roughly 10 % of the time (SHA-256 and zstd), so a 14 GiB NUP takes about 8 minutes when the game is quiet.

## What the game reads

Every guest read of a storage (`IStorage`), a file (`IFile`) or a NUP member is counted against its source: two relaxed adds into a per-source slot and two into the totals, no lock, no allocation, no clock. The name of a source is looked up once, on the first read of an open handle. When a 10 s window holds 500 or more reads, or 64 MiB or more, of one kind, the report loop writes one line with the top 5 sources by bytes:

```
Game reads in the last 10.0 s: storage 812 reads 104.3 MiB, nup 1420 reads 141.8 MiB. Top sources by bytes: [nup] chr_pc_001.wismt 120 reads 48.1 MiB; [nup] bf2_ev.arh 9 reads 31.0 MiB; [file] /saves/user.bin 400 reads 2.1 MiB
```

- The kinds overlap (a read of the data storage is served by reads of NUP members or files), so they are listed side by side and never added.
- `storage` names are `data storage of the current process`, `data <title> on storage <id>` and so on; `file` names are the full path in the guest's file system; `nup` names are the member name.
- Past 4094 distinct sources the rest are counted together as `(other sources)`.
- A quiet window writes nothing.
