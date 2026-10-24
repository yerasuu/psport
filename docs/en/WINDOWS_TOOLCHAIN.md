# The Windows toolchain package

decanter builds a psport branch on Windows without MSYS2 (kb #926, decanter's decisions #924 and
#928): it downloads llvm-mingw, CMake and Ninja from upstream and psport's toolchain package, checks
each against a pinned SHA-256, and runs `cmake` and `ninja` on psport's sources. The package holds
everything else the build needs, so that it needs no network: MSYS2's CLANG64 packages pinned by
SHA-256, psport's LGPL FFmpeg, the sources CMake would otherwise fetch, and a CMake toolchain file.

## Tag and assets

A tag `toolchain-win32-N` (`N` = 1, 2, ...) runs `.github/workflows/toolchain.yml`, which publishes
in the release of that tag, where anyone downloads it without an account:

| Asset | Contents |
|---|---|
| `psport-toolchain-win32-N-x86_64.zip` | the package (below) |
| `psport-toolchain-win32-N-x86_64.json` | its `manifest.json`, to read the pinned tools before downloading the zip |
| `psport-toolchain-win32-N-x86_64.zip.sha256` | `<sha256>  <zip name>` (`sha256sum` format), uploaded last |

`https://github.com/yerasuu/psport/releases/download/toolchain-win32-N/<asset>`. A new `N` is tagged
whenever a package, FFmpeg or a pinned tool changes; older releases stay, so a decanter that pins one
keeps working. The release is never marked latest. A release that already has the `.sha256` keeps
its files when the workflow runs again (`tools/win32-publish.sh`). The tag needs no version bump: it
names the package, not a psport release, and any psport commit from the one that added
`CMakeLists.txt` on builds with it.

The workflow ([filter patterns](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#filter-pattern-cheat-sheet):
`toolchain-win32-[0-9]+`) runs four jobs:

1. `ffmpeg` (`windows-latest`, MSYS2 CLANG64): `packaging/windows/ffmpeg/PKGBUILD` with
   `makepkg-mingw`, as `windows.yml` builds it. A tag builds it afresh, so that what it publishes rests
   on no cache entry another run wrote; a run by hand may take `windows.yml`'s cached package.
2. `package` (`ubuntu-latest`): `tools/win32-cross.sh sysroot`, `tools/win32-cross.sh ffmpeg-package
   <that package>` (which takes exactly one), `tools/win32-toolchain.sh dist N`.
3. `check` (`windows-latest`, PowerShell, no MSYS2): what decanter does. It downloads the three tools
   from the manifest's URLs, compares their SHA-256, unpacks them and the package with Windows'
   `tar.exe`, runs `cmake` and `ninja` with only `System32` on `PATH`, then `psport.exe
   --list-features` and `psport-windows-probe.exe`.
4. `publish` (`ubuntu-latest`, the only job with `contents: write`, only for tag pushes of
   `yerasuu/psport`): checks the zip against the `package` job's SHA-256 and uploads the assets.

By hand (`workflow_dispatch`, with a number) it builds and checks a package without publishing it;
the zip is the run's artifact for 14 days.

## Layout

The zip holds one folder, `psport-toolchain-win32-N-x86_64/`:

| Path | Contents |
|---|---|
| `manifest.json` | below |
| `psport-toolchain.cmake` | the CMake toolchain file (`packaging/windows/toolchain/`) |
| `clang64/` | the CLANG64 packages, each unpacked from its archive: headers, import libraries, DLLs, `pkgconf.exe` and `glslang.exe` with the DLLs they load, licences in `clang64/share/licenses/` |
| `usr/share/` | the few files a package installs outside `clang64/` |
| `sources/magic_enum/`, `sources/miniz/` | the release tags `gpu/CMakeLists.txt` fetches (`GIT_TAG`), unpacked |
| `sources/mingw-w64-ffmpeg-lgpl-<version>/` | FFmpeg's PKGBUILD, release key and the upstream tarball and signature it builds from |
| `SOURCES.txt` | the written offer and each package's source (MSYS2's source package, or the folder above) |
| `LICENSE.txt` | psport's GPL-2.0, for its own files in the package |

`tools/win32-toolchain.sh` refuses a package with links, names Windows reserves or cannot hold
(`CON`, `AUX.h`, `:`, a trailing dot or space) or names that differ only in case. The zip's entries
are in byte order with the commit's time: the same sysroot, FFmpeg package, commit and `zip` program
give the same zip. FFmpeg built again by `makepkg-mingw` is not the same bytes, so a rerun of the
workflow gives another zip, which is why a published `.sha256` is never replaced. Toolchain 1 measured
on Linux: 316 968 482 bytes zipped, 1 063 146 929 bytes unpacked, 51 863 files, the longest path
inside 159 characters (with the top folder).

## Where the hashes come from

Each MSYS2 package's SHA-256 in `manifest.json` is the one in MSYS2's database, `clang64.db`, read at
the time the tag ran, and `tools/win32-toolchain.sh` unpacks only archives that match it.
`tools/win32-cross.sh sysroot` accepts that database only with a good signature
(`clang64.db.sig`, checked with gpg) from one of MSYS2's master keys or from a key that one of them
certified, which is how pacman trusts packagers. The master keys' fingerprints are pinned in the
script, as `msys2-keyring` lists them in `msys2-trusted` (the same in
[MSYS2-keyring](https://github.com/msys2/MSYS2-keyring/blob/master/msys2-trusted)); the keyring
package downloaded from repo.msys2.org only supplies the keys themselves, so a keyring or database
altered in transit fails the check. A signer the keyring lists as revoked is refused. The manifest
records the database's SHA-256 and the signing key (`msys2_database`). What this does not cover: an
older database correctly signed (replayed) would be accepted, and a key revoked after the keyring
release that the script downloads still counts. FFmpeg's hashes come from its PKGBUILD (the upstream
tarball, whose signature the FFmpeg release key in `keys/pgp/` checks), magic_enum's and miniz's are
pinned in `tools/win32-toolchain.sh`.

## manifest.json

```json
{
  "schema": 1,
  "name": "psport-toolchain-win32-N-x86_64",
  "tag": "toolchain-win32-N",
  "number": N,
  "commit": "<the 40-hex psport commit that made it>",
  "target": "x86_64-w64-windows-gnu",
  "llvm_major": 23,
  "toolchain_file": "psport-toolchain.cmake",
  "sysroot": "clang64",
  "msys2_database": {"file": "clang64.db", "sha256": "...", "signer": "<signing key fingerprint>"},
  "tools": [
    {"name": "llvm-mingw", "version": "...", "url": "...", "sha256": "...", "size": <bytes>,
     "folder": "<the folder the zip unpacks into, empty when the files sit at its top>", "page": "..."}
  ],
  "packages": [
    {"name": "mingw-w64-clang-x86_64-...", "version": "...", "file": "<archive>", "sha256": "...",
     "url": "<repo.msys2.org archive, null for one psport builds>", "origin": "MSYS2 | psport's <PKGBUILD>",
     "licence": "...", "source": "<source package URL or folder in the zip>", "upstream": "..."}
  ],
  "sources": [
    {"name": "...", "version": "...", "url": "...", "sha256": "...", "licence": "...", "path": "<in the zip>"}
  ]
}
```

`schema` changes when a field changes meaning or goes away; fields may be added without it. Every
`sha256` is lowercase hexadecimal.

## The pinned tools

decanter downloads these itself; none is in the package. Toolchain 1 pins:

| Tool | Version | Asset | SHA-256 | Where the hash comes from |
|---|---|---|---|---|
| llvm-mingw | 20261020 (LLVM 23.1.4, UCRT) | [llvm-mingw-20261020-ucrt-x86_64.zip](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261020) | `fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd` | the release asset's digest in GitHub's API, matching the download; the project publishes no checksum file |
| CMake | 4.4.4 | [cmake-4.4.4-windows-x86_64.zip](https://cmake.org/download/) | `bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536` | `cmake-4.4.4-SHA-256.txt`, signed by Kitware (Brad King, `CBA23971357C2E6590D9EFD3EC8FEF3A7BFB4EDA`), checked |
| Ninja | 1.13.2 | [ninja-win.zip](https://github.com/ninja-build/ninja/releases/tag/v1.13.2) | `07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65` | the release asset's digest in GitHub's API, matching the download; no checksum file upstream |

llvm-mingw's `clang` must be the LLVM major of CLANG64's `compiler-rt` and libc++ headers
(`llvm_major`); `tools/win32-toolchain.sh` stops when CLANG64 has moved on, and the pins move with a
new `N`. nasm is not pinned: only FFmpeg needs it, and FFmpeg comes built.

## Building with it

From a psport checkout at a commit with `CMakeLists.txt`, with `gpu/third_party/imgui` checked out
(`git submodule update --init gpu/third_party/imgui`; Tracy is not needed):

```
<cmake>/bin/cmake.exe -S <checkout> -B <build> -G Ninja
    -DCMAKE_MAKE_PROGRAM=<ninja>/ninja.exe
    -DCMAKE_TOOLCHAIN_FILE=<package>/psport-toolchain.cmake
    -DPS_LLVM_MINGW=<llvm-mingw>/llvm-mingw-20261020-ucrt-x86_64
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS_LTO=OFF -DPS_COMMIT=<git rev-parse --short=12 HEAD>
<ninja>/ninja.exe -C <build>
```

- Every path absolute, `/` or `\`. `PS_LLVM_MINGW` is the folder the manifest names inside
  llvm-mingw's zip; the toolchain file takes `clang`, `clang++`, `llvm-ar`, `llvm-ranlib`,
  `llvm-windres` and `llvm-dlltool` from its `bin/`, and `pkgconf.exe` and `glslang.exe` from the
  package's `clang64/bin/`, never from `PATH`. `PATH` needs only `%SystemRoot%\System32`: Ninja runs
  CMake's custom commands (the GPU library's shaders, `psport.exe`'s import library) through
  `cmd.exe /C`.
- The toolchain file sets the pkg-config variables itself (`PKG_CONFIG_LIBDIR`,
  `PKG_CONFIG_SYSROOT_DIR` to the package, `PKG_CONFIG_DONT_DEFINE_PREFIX`, since FFmpeg's and fmt's
  `.pc` files name `/clang64` outright; [pkgconf](https://github.com/pkgconf/pkgconf)), and turns
  FetchContent off the network with magic_enum and miniz from `sources/`
  ([FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html):
  `FETCHCONTENT_FULLY_DISCONNECTED`, `FETCHCONTENT_SOURCE_DIR_<NAME>`). clang runs with
  `--no-default-config`, so llvm-mingw's own `.cfg` files and sysroot play no part
  ([clang configuration files](https://clang.llvm.org/docs/UsersManual.html#configuration-files)).
- `PS_LTO=ON` (the default) links the GPU library with LTO, slower; the CI's test package is built
  without. `PS_COMMIT` is what the programs report; empty without it. `VERSION` must be `0-unstable`
  or `X.Y.Z` with `tag-changelog/vX.Y.Z.md` written, as for `build.sh`.
- Keep the paths short. Ninja on Windows refuses a file whose full path passes 260 characters, and
  the longest file in the checkout is 190 characters below its root (FSR's generated headers), so
  the checkout's own path must stay within about 68 characters. CMake warns when an object's full
  path may pass 250 characters (`CMAKE_OBJECT_PATH_MAX`); the deepest objects add about 150 to the
  build folder. Spaces in the checkout's, the build folder's or the package's path are fine.

`<build>` then holds `psport.exe`, `psport-gpu-capabilities.exe`, `psport-windows-probe.exe` and
`gpu/libpsport-gpu.dll`. To run them, put `libpsport-gpu.dll` next to `psport.exe` (or its folder on
the DLL search path) together with the DLLs they import from `clang64/bin/` (`SDL3.dll`, `libc++.dll`,
FFmpeg's, ...: `tools/win32-bundle.sh` lists the closure); `vulkan-1.dll` is the GPU driver's.

## The same from Linux

```sh
tools/win32-cross.sh sysroot            # CLANG64 packages into PS_WIN32_SYSROOT
tools/win32-cross.sh ffmpeg             # or: ffmpeg-package <the CI's .pkg.tar.zst>
tools/win32-toolchain.sh dist 1         # dist/psport-toolchain-win32-1-x86_64.zip, .json, .sha256
```

`tools/win32-toolchain.sh` needs `curl`, `jq`, `zip`, `zstd` and `sha256sum`. To check a package
from Linux, unpack it and llvm-mingw's Linux build of the same release
(`llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64.tar.xz`), and run the commands above with Linux's
CMake and Ninja; the toolchain file then takes the host's `pkg-config` and `glslang` (pkg-config
0.29's flags break at a space in the package's path, which pkgconf on Windows escapes). The programs
run under Proton with `tools/win32-proton-run.sh` (`PS_WIN32_SYSROOT=<package folder>`).

## What it was checked with

Toolchain 1 at psport `03d2f2598`, on Linux: built from the package with llvm-mingw 20261020's
Linux build, CMake 4.4.4 and Ninja 1.13.2, and again with the Windows builds of the three under
Proton 10 (Wine), which ran `pkgconf.exe` and `glslang.exe` from the package (a minute each with
32 threads, without LTO). Both give programs that import the same DLLs, and a `psport.exe` that
exports the same symbols as `tools/win32-cross.sh`'s. Under Proton, their `--list-features` and the
probe's report matched those of the CI's test package for that commit (paths, times and free memory
aside), also with the four programs dropped into the CI's package in place of its own. The Windows
tools built it again under Wine with a space in the checkout's, the build folder's and the package's
paths. What only
GitHub shows: the workflow itself, the `check` job on a real Windows, and the release assets.
