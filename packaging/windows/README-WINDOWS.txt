psport for Windows: test package
================================

This package is for testing, not for playing. psport's Windows port is groundwork: under Wine with
AMD's Linux driver it stops at "Guest memory: direct memory cannot be shared with this driver",
because that driver does not let the GPU use memory the way psport maps the PS4's memory. Whether
real Windows drivers (NVIDIA, AMD, Intel) allow it is what this package finds out. BUILD-INFO.txt
says which commit it was built from and what it holds.

What you need
-------------
- Windows 10 version 1803 or later, or Windows 11, 64-bit.
- A GPU driver with Vulkan, from NVIDIA, AMD or Intel (the one Windows Update installs usually has
  it). No game, no PS4 files and no administrator rights are needed.

Steps
-----
1. Extract the whole zip into a folder of your own, for example your Desktop (right-click the zip >
   Extract All...). Running the files from inside the zip does not work.
2. Double-click psport-probe.bat. If Windows says "Windows protected your PC", click "More info"
   then "Run anyway": the programs are not signed.
   (Or, in PowerShell: right-click psport-probe.ps1 > Run with PowerShell.)
3. A console window shows the probe working for a few seconds; then Notepad opens
   psport-windows-report.txt, written next to the programs (or in %TEMP% when that folder is not
   writable; the console prints where).
4. Send psport-windows-report.txt back, as it is. If no report was written, send a screenshot of the
   console window.

What the probe does
-------------------
psport-windows-probe.exe only reads system information and runs short tests; it changes nothing on
the machine and needs no network. The report holds:
- Windows' version and build, the CPU, RAM, and each display adapter's driver as Windows lists it.
- The PS_* and VK_* environment variables, and the Vulkan layers installed (overlays and capture
  tools load as layers and can change results).
- For each Vulkan device: name, vendor and device ids, driver name and version, Vulkan version, the
  extensions and features psport relies on, its memory heaps and types, ReBAR size.
- An active test of each way the GPU could share memory with psport, with PASS or FAIL and Vulkan's
  result code: [a] private memory, [b] and [b2] views of a shared memory section, [c] a view placed
  at a chosen address (psport's way), [d] two views of one section at once, [e1] and [e2] Vulkan
  memory exported and imported through Windows handles; and whether one section shows the same data
  at two addresses.
- The output of psport-gpu-capabilities.exe --gpus and psport.exe --list-features.
- A Verdict: whether psport's guest-memory mode would work on this machine and, if not, which
  alternative would.

If something goes wrong
-----------------------
- "vulkan-1.dll does not load" in the report: install the GPU vendor's current driver.
- psport.exe --list-features ending with exit code 0xc0000135: a DLL psport.exe needs is not in the
  package (the report names it); the probe's own results are complete without it.
- A line starting with CRASH: the report is cut at the step it names; send it anyway.

Running psport.exe by hand
--------------------------
From a console in this folder: psport.exe --list-features lists what works on Windows. Games are
not expected to run yet.

Licences
--------
psport is GPL-2.0 (LICENSE.txt). The DLLs next to it and the code compiled into it are listed with
their licences in THIRD-PARTY-NOTICES.txt; where their sources are, in SOURCES.txt. FFmpeg's DLLs
(avcodec, avformat, avutil, swresample, swscale) are FFmpeg under the GNU LGPL version 2.1 or later,
built by psport without changes to its source; THIRD-PARTY-NOTICES.txt has the configure line, and
the PKGBUILD and source it was built from come next to this package's zip (SOURCES.txt). You may
replace those DLLs with your own build of the same FFmpeg version.
