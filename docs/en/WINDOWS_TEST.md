# Testing psport on Windows

psport's Windows port is groundwork (#614): `psport.exe` links and, under Proton on AMD's RADV,
gets as far as its window and swapchain, then stops at
`Guest memory: direct memory cannot be shared with this driver`. psport maps the PS4's direct
memory as views of one section at several addresses and lets the GPU import them with
`VK_EXT_external_memory_host`; RADV under Wine imports private memory only (kb #870, #872).
Whether NVIDIA, AMD or Intel's Windows drivers import section views decides how the port goes on
(decision #873). The test package answers that on a real machine without a game.

## Get the package

Each release `vX.Y.Z` carries the package where anyone downloads it without an account (kb #920),
and decanter on Windows takes it from there: `psport-windows-vX.Y.Z-x86_64.zip` and
`psport-windows-vX.Y.Z-x86_64.zip.sha256` (`<sha256>  <zip name>`, `sha256sum` format):

```sh
tag=v0.1.5
base=https://github.com/yerasuu/psport/releases/download/$tag
curl -fLO "$base/psport-windows-$tag-x86_64.zip"
curl -fLO "$base/psport-windows-$tag-x86_64.zip.sha256"
sha256sum -c "psport-windows-$tag-x86_64.zip.sha256"
```

On Windows, PowerShell's `Get-FileHash <zip>` prints the SHA-256 to compare (in capitals).

Only tags publish (decanter's decision #925): a branch is tested from its sources, built locally. A
tag's assets appear once its workflow run finishes, before or after the Linux ones. The zip holds one
folder, `psport-windows-<version>-<commit>-x86_64/` (the 12-character commit), whatever the asset's
name.

Every push also keeps the run's artifacts: in the repository's Actions tab (workflow "Windows"),
`psport-windows-<version>-<commit>-x86_64.zip` is the same package, and
`psport-windows-<version>-<commit>-x86_64-sources` holds the PKGBUILD and upstream tarball of the
FFmpeg it carries. GitHub keeps both 14 days and asks for a signed-in account.

Without CI, from Linux: `tools/win32-cross.sh sysroot`, `ffmpeg`, `build`, `runtime`, then
`tools/win32-bundle.sh DEST` writes the same zip and sources folder (see
[FORK.md](../../FORK.md#ci-and-binary-cache)).

## Run it

1. Extract the whole zip into a folder (Explorer: right-click > Extract All...).
2. Double-click `psport-probe.bat` (or right-click `psport-probe.ps1` > Run with PowerShell).
   SmartScreen warns about unsigned programs: More info > Run anyway.
3. After a few seconds Notepad opens `psport-windows-report.txt`, next to the programs (or in
   `%TEMP%` when that folder is not writable).
4. Send that file back as it is.

`README-WINDOWS.txt` in the package says the same for testers, and `BUILD-INFO.txt` which commit
it is and which DLLs it carries.

## What the report holds

`psport-windows-probe.exe` (`tools/win32_probe/`) loads Vulkan at run time, so a machine without a
driver still gets a report, and writes a crash inside a driver down with the step it hit.

- System: Windows version and build, Wine when it runs under it, CPU, RAM and commit limit,
  `VirtualAlloc2` and `MapViewOfFile3`, and each display adapter's driver as the registry lists it.
- The `PS_*` and `VK_*` variables and the Vulkan layers installed.
- Each Vulkan device: name, vendor and device ids, driver name, info and conformance version,
  Vulkan version, the extensions psport looks at (`VK_EXT_external_memory_host` with
  `minImportedHostPointerAlignment`, `VK_KHR_external_memory_win32`, `VK_EXT_map_memory_placed`
  with its features and alignment, ...), Vulkan 1.2 and 1.3 features (buffer device address,
  descriptor indexing, ...), memory heaps with their budget, memory types, ReBAR size, which
  external handle types buffers can export or import, and the full extension list.
- Memory sharing tests on each device, 1 MiB each, PASS or FAIL with the `VkResult` and the step
  that failed. An import passes only when the GPU reads what the CPU wrote and the CPU sees what the
  GPU wrote:
  - `[a]` host import of private `VirtualAlloc` memory;
  - `[b]` of a `MapViewOfFile` view of a pagefile section, `[b2]` of a `SEC_RESERVE` one;
  - `[c]` of a `MapViewOfFile3` view placed into a placeholder, psport's guest mapping;
  - `[d]` of two views of one section at different addresses, both imported at once, the writes
    crossing from one view to the other;
  - `[e1]` and `[e2]` `OPAQUE_WIN32` and `OPAQUE_WIN32_KMT` export and import round trips, and
    whether the exported handle maps on the CPU.
  Without the GPU: whether one section shows the same data at two addresses.
- The output of `psport-gpu-capabilities.exe --gpus` and `psport.exe --list-features`, and the DLLs
  `psport.exe` needs that neither the folder nor Windows has.
- Verdict, for the device psport takes (its own ranking or `PS_GPU_ID`): whether the current
  guest-memory mode would work (`[c]`, `[d]` and the CPU alias pass), else whether private memory
  would (decision #873 option 2) or only a copy path (option 3), with the facts behind it; one line
  for every other device.

## Known limits

- FFmpeg is psport's own build (`packaging/windows/ffmpeg/PKGBUILD`, kb #906), LGPL-2.1-or-later
  with only the decoders, parsers, demuxer and protocol psport uses (H.264, HEVC, AAC, MP3, MJPEG;
  MP4 through the mov demuxer; local files), since MSYS2's is GPL-3.0-or-later, which GPL-2.0 psport
  can't be redistributed with. A video in another codec or container than these fails to open; the
  Linux build has the same decoders and more.
- Under Proton on RADV the report says what kb #872 found: `[a]`, `[e1]`, `[e2]` and the CPU alias
  pass, `[b]` to `[d]` fail with `VK_ERROR_INVALID_EXTERNAL_HANDLE`.
