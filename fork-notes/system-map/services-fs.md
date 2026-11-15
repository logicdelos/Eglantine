# File system, containers, loaders and keys (services-fs)

**Purpose.** This slice turns the files the user points at (loose directories, NSP, XCI, NCA, NRO, and the fork's `.nup` package) into the `VfsFile`/`VfsDirectory` trees the kernel and the guest see. It implements the `fsp-srv` IPC services (RomFS, saves, SD card, storages) on top of those trees. It also holds the registered content cache (NAND installs, add-on content), the patch layer (update, mods, layered FS) and the key store and NCZ decompression. Switch 2 `.nup` support is fork code (`nup_container.*`, `nup_writer.*`, `loader/nup.cpp`, boot content in `romfs_factory.cpp`).

## Files

| file | role | read depth |
|---|---|---|
| `file_sys/vfs/vfs_real.cpp/.h` | host-disk VFS with a FILE* LRU and a path-to-file cache | full |
| `file_sys/vfs/vfs.cpp` | VfsFile/VfsDirectory defaults, `ReadBytes`, `VfsRawCopy` | partial |
| `vfs_concat/offset/layered/cached/vector` | wrapper files and dirs | not opened (used via callers) |
| `hle/service/filesystem/filesystem.cpp/.h` | `FileSystemController`, `VfsDirectoryServiceWrapper`, `LoopProcess` | full |
| `filesystem/fsp/fsp_srv.cpp` | `FSP_SRV` command table and handlers | partial (35-672 skimmed fast) |
| `fsp/fs_i_filesystem.cpp`, `fs_i_file.cpp`, `fs_i_storage.*`, `fs_i_directory.cpp` | per-handle IPC objects | full |
| `fsa/fs_i_file.h`, `fsa/fs_i_filesystem.h` | SDK-style fs interfaces | full |
| `filesystem/romfs_controller.cpp`, `file_sys/romfs_factory.cpp` | RomFS open, update merge, boot content (fork) | full |
| `file_sys/romfs.cpp` | `ExtractRomFS`, `CreateRomFS` | partial (60-170) |
| `file_sys/patch_manager.cpp` | update, mods, layered FS, control data | partial (253-473, 655-863) |
| `file_sys/registered_cache.cpp` | content provider, install, `Refresh` | partial (36-333, 559-1263) |
| `file_sys/nup_container.*` | NUP parser, tree builder, zstd cache (fork) | full |
| `loader/nup.cpp` | NUP loader (fork) | full |
| `file_sys/nup_writer.cpp` | NUP pack and extract tool used by the GUI (fork) | skimmed (outline only) |
| `crypto/key_manager.cpp/.h` | key files, derivation | partial (559-885 read; derivation 885-1160 not read) |
| `crypto/ncz_decompressor.cpp` | NCZ block and solid readers | full |
| `file_sys/savedata_factory.cpp`, `filesystem/save_data_controller.cpp` | save paths and sizes | full |
| `hle/service/aoc/addon_content_manager.cpp` | AOC list, boot content merge | partial (59-153) |
| `fssystem/*` (NCA driver, bucket tree, compressed storage, integrity) | NCA section decrypt, patch storages | not opened |
| `card_image`, `submission_package`, `content_archive`, `loader/{loader,nca,nsp,xci,nso,nro,deconstructed_rom_directory}.cpp` | containers and loaders | not opened except grep hits |
| `ro`, `pm`, `spl`, `es`, `ncm`, `pctl`, `bcat/news/*` services, `partition_data_manager`, `aes_util` | other services and crypto | not opened |

## Main types and ownership

- `FileSystemController` (owned by `Core::System`) owns `registrations` (per-process `{program_id, RomFSFactory, SaveDataFactory}`), `sdmc_factory`, `bis_factory`, the external `ContentProviderUnion` and the mounted gamecard `XCI` with its caches. `RegisterProcess` uses `emplace`, so it never overwrites an existing entry.
- `FSP_SRV` is created per client connection. `SetCurrentProcess` calls `fsc.OpenProcess`, which builds a fresh `SaveDataController` and `RomFsController` for that session.
- `IFileSystem`, `IFile`, `IStorage`, `IDirectory` are per-handle service objects. Each wraps a `VirtualFile` or `VirtualDir` plus a `VfsDirectoryServiceWrapper` (path sanitising, guest-path to VFS mapping).
- `RealVfsFilesystem` owns `cache` (`std::map<path, weak_ptr<VfsFile>>`) and two intrusive lists (`open_references`, `closed_references`) forming an LRU of open FILE* handles. Each `RealVfsFile` owns a `FileReference` and a cached optional size. A loose RomFS therefore keeps one `RealVfsFile` per file.
- `NupContainer` owns the parsed TOC (`packages`, `dirs`, `files`, `children`), `exefs_dir_cache`/`romfs_dir_cache`, and a `shared_ptr<NupDecompressCache>` shared with every `NupVfsFile`. The cache owns the LRU of decompressed members (default cap 512 MiB), plus `spills`: per-member temp files under `CacheDir/nup`, deleted in `~NupSpillFile`.
- `RomFSFactory` keeps a process-global boot-content table (`boot_content`, `boot_content_romfs`) set by the NUP loader and read by `OpenBootContent`, `OpenPatchedRomFSWithProgramIndex` and the AOC service.
- `KeyManager` is a function-local static (`Instance()`), filled in its constructor by `ReloadKeys`.

## Threads and locking

- All `fsp-srv`, `fsp-ldr`, `fsp-pr` sessions are served by one `ServerManager` thread. A blocking call (for example the `IStorage` throttle sleep) delays every fs request, including saves.
- `RealVfsFilesystem::list_lock` guards `cache`, both reference lists and `num_open_files`. `RefreshReference` returns the held lock, and `RealVfsFile::Read/Write/GetSize` keep it for the whole Seek+Read, so reads of every loose file serialise on one mutex. `RealVfsFile::size` (a `std::optional`) is reset in `Write`/`Resize` and read in `GetSize` without the lock.
- `FileSystemController::registration_lock` guards `registrations`. `RomFSFactory::layered_mutex` guards the cached layered RomFS. `boot_content_mutex` guards the global boot-content table.
- `NupDecompressCache::mutex` guards `lru`, `index`, `bytes`, `spills`. The decompress itself runs unlocked; `NupSpillFile::mutex` serialises decompress-to-file and reads of one spilled member. Lock order observed: cache mutex is released before the spill mutex is taken.
- `BlockNczFile` and `SolidNczFile` each have a mutex around their cursor/cache state. `KeyManager` has no lock; it is mutated by `SetKey`/`DeriveBase` and read on loader and install threads.
- `ASSERT_MSG` is soft: `AssertFailSoftImpl` returns unless `Settings::values.use_debug_asserts` is set. Several fs "asserts" therefore only log and continue.

## Flow

1. Boot: `Loader::GetLoader` picks `AppLoader_NUP`, `_NSP`, `_XCI`, `_NCA`, `_NRO` or `_DeconstructedRomDirectory` by file type. Each `Load` registers a `RomFSFactory` for the process via `FileSystemController::RegisterProcess`.
2. NUP: the `AppLoader_NUP` constructor builds `NupContainer` (`Parse`: superblock, TOC SHA-256, bounds checks on all records, `ValidateDirTree` cycle/depth check, `ParseMeta` TLVs). It picks the base/update package, wraps its ExeFS in `AppLoader_DeconstructedRomDirectory`, builds a NACP and icon. `Load` runs the secondary loader, then `SetBootContent` with add-on and sibling-program RomFS dirs, then `RegisterProcess`.
3. Guest asks for RomFS: `FSP_SRV::SetCurrentProcess`, then `OpenDataStorageByCurrentProcess` -> `RomFsController::OpenRomFSCurrentProcess` -> `RomFSFactory::OpenCurrentProcess` (update/layered merge by `PatchManager::PatchRomFS`) -> `IStorage`. `OpenDataFileSystemByCurrentProcess` instead calls `FileSys::ExtractRomFS` on each call to build a directory tree. `OpenDataStorageWithProgramIndex` falls back to the current process when the requested program is absent (fork).
4. Reads: `IStorage::Read` or `IFile::Read` -> `VfsFile::Read` chain (`OffsetVfsFile` over `RealVfsFile` for loose RomFS, `NupVfsFile` for zstd NUP members, NCA storages for installed content). `IStorage::Read` first applies the fork's read-rate throttle.
5. Saves: `OpenSaveDataFileSystem` -> `SaveDataController::OpenSaveData` -> `SaveDataFactory::Open` (path from `GetFullPath`, auto-create for cache/temp/account/device) -> `IFileSystem` over a real directory. Temporary, ProperSystem and SafeMode spaces are mapped to NAND storage ids instead of asserting.
6. Install: `RegisteredCache::InstallEntry` / `RawInstallNCA` copy NCAs (NCZ via `DecompressNCZForInstall`), `Refresh` rescans the directory and rebuilds NCA objects.

## Connections

- Called by: the loaders (`AppLoader::Load` calls `RegisterProcess`), `aoc` (`ListEntriesFilter`, `ListBootContent`), `core.cpp` (`SetBootContent({})` on reset), the frontends (`frontend_common/content_manager.h` calls `VerifyIntegrity`; GUI install and `nup_writer`). Other service callers of `ContentProvider` (`ncm`, `es`) were not opened.
- Calls out to: `Common::FS` (host files), `Core::Crypto::KeyManager` (NCA key derivation), OpenSSL (`EVP_Digest`), zstd, `Settings::values`, `Core::System::ApplicationProcess` (`IsSwitch2` for the throttle), `Kernel::KProcess` for process id lookups.

## Settings and switches

- `Settings::values.enable_fs_access_log` (fs access logging), `external_content_dirs` (extra registered caches), `dump_exefs`, `disabled_addons` (per title; "DLC" key checked by the NUP loader and `aoc`), `use_dev_keys` (selects dev.keys vs prod.keys), `use_debug_asserts` (makes ASSERT_MSG break).
- Environment: `EDEN_NX2_FS_READ_MBPS` (`IStorage` throttle in MB/s; 0 disables; unset = 40 MB/s, or 100 MB/s for Switch 2 processes).
- Windows: `yuzu/main.cpp` raises the CRT stdio limit (`_setmaxstdio` 2048 then 8192) and `RealVfsFilesystem::MaxOpenFiles()` derives its handle cap from it.

## Invariants

- NUP offsets, extents, names and child indices are bounds-checked in `Parse` before any use (enforced), tree depth is capped at `MaxDirDepth` (enforced), the TOC hash is checked (enforced), but member contents are only checked by `VerifyIntegrity` (on demand).
- A `RealVfsFile` handle is valid only while its `FileReference` is in a list under `list_lock` (enforced by `RefreshReference`).
- `ExtractRomFS` assumes meta offsets are in range and acyclic (only assumed; see findings).
- Guest buffer length equals the `size` argument of `IFile::Read`/`IStorage::Read` (only assumed).
- A RealVfsFile cache hit returns the existing object regardless of the requested permissions (an existing read-only instance may be returned to a writer).
- `NupContainer::OpenFile` returns an empty `VectorVfsFile` for size 0; raw members are zero-copy `OffsetVfsFile`s.

## Hazards and improvement candidates

See `findings/services-fs.json`. Short list:

- Cross-directory `RenameFile` null dereference; soft asserts hide failed writes (`DoWrite`).
- `ExtractRomFS` has no guard against truncated or cyclic metadata. `OpenDataFileSystemByCurrentProcess` rebuilds the whole tree per call, which matters for loose RomFS with ~200k files.
- Guest read length is not clamped to the host scratch buffer.
- Key file parsing indexes 0x20-entry arrays with values up to 0xFF.
- NCZ: unchecked block count allocation; solid NCZ install keeps the whole NCA in RAM (since `39216de` it goes to a temporary file under `<cache>/ncz/`, and `OpenNcz` returns null if that file cannot be written).
- NUP zstd members decompress whole per miss with no de-duplication (since `39216de` a member is decompressed by one thread at a time and the others take the cached copy); spill files are never evicted.
- `list_lock` held across disk reads in `RealVfsFile::Read` (since `39216de` only for files opened writable; a read-only file reads under its own `io_mutex`); `RegisteredCache::GetFileAtID` probes five layouts and each probe hashes with SHA-256; `FollowsTwoDigitDirFormat` uses `std::regex`.
- `ApplyLayeredFS` extracts and rebuilds the entire RomFS whenever any layer exists.
- `InstallNSP` (`frontend_common/content_manager.h`) installed a truncated file after a short read or write; since `39216de` it fails. `InstallNCA` in the same header fails the same way since `44bbe5a`. `VfsRawCopy` (`vfs/vfs.cpp:507`), which `registered_cache.cpp:464` calls, already failed on a short read or write.
- Not opened: `fssystem/*`, `bcat` news msgpack, `ro`, `pm`, `spl`, `es`, `ncm`, `pctl`, most loaders; no claims are made about them.
