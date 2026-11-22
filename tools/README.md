# Tools

Tools for Eden and other subprojects. When adding new scripts please use `#!/bin/sh -e` or `#!/usr/bin/env <interpreter>` (for `.py`, `.rb`, or `.perl`). Keep scripts POSIX compliant (i.e not require hard `bash` to run, just plain old `sh`).

## Third-Party

- [CPMUtil Scripts](./cpm)

## Binaries

- `maxwell-spirv`: Converts Maxwell shaders (dumped from `.ash` files) into SPIR-V code (emitted into STDOUT).
- `maxwell-disas`: Dumb raw Maxwell disassembler.
- `maxwell-ir`: Dump generated IR of Maxwell shaders.

## Scripts

- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `shellcheck.sh`: Ensure POSIX compliance (and syntax sanity) for all tools in this directory and subdirectories.
- `llvmpipe-run.sh`: Sets environment variables needed to run any command (or Eden) with llvmpipe.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- `update-cpm.sh`: Updates CPM.cmake to the latest version.
- `update-icons.sh`: Rebuild all icons (macOS, Windows, bitmaps) based on the master SVG file (`dist/dev.eden_emu.eden.svg`)
    * Also optimizes the master SVG
    * Requires: `png2icns` (libicns), ImageMagick, [`svgo`](https://github.com/svg/svgo)
- `dtrace-tool.sh`
- `lanczos-gen.pl`: Generates constants for the Lanczos filter.
- `clang-format.sh`: Runs `clang-format` on the entire codebase.
    * Requires: clang
- `find-unused-strings.sh`: Find any unused strings in the Android app (XML -> Kotlin).
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `fuzzsettings.cpp`: Fuzz settings files.

### Isolated in-game saves for testing

`save_timeline.py` (Python 3.11+) copies only `user/save`, `system/save` and `temp` from the configured save directory. Stop **every** writer to that directory first; hashing is not a filesystem freeze. Use separate directories outside the source:

```text
python tools/save_timeline.py capture <SaveDir> <generation-store>
python tools/save_timeline.py branch <printed-generation-directory> <branch-store>
```

Each `branch` checks the recorded digest before publishing a new writable directory. In a disposable test profile, set **save_directory** to that printed branch path. Repeating `branch` creates another independent copy. Never point the emulator at the generation itself. These files contain private game saves; they do not belong in shared diagnostic reports. No SD-card data, CPU, kernel, GPU or audio state is captured: this cannot resume a cutscene.

## Android

It's recommended to run these scripts after almost any Android change, as they are relatively fast and important both for APK bloat and CI.

- `unused-strings.sh`: Finds unused strings in `strings.xml` files.
- `stale-translations.sh`: Finds translated strings that aren't present in the source `strings.xml` file.

## Translations

- [Translation Scripts](./translations)
