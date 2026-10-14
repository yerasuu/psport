# File mods, third-party patches and TAA sharpening

These features come from bbport and are built around Bloodborne: the mod layouts below are those
of its `dvdroot_ps4` folder, and patches are compiled only for Bloodborne's 1.09 executable
(`scripts/patches.py` writes an empty patch file for any other title).

## File mods

The launcher has a **Mods** group: folder selection, a master switch, a switch per mod and arrows
for the load order. Changes apply on the next launch. The lowest enabled mod has the highest
priority; when files collide, the log says which mod replaced the previous one.

The default folder is `mods/` in the data directory (`BB_DATA_DIR`, the checkout by default); in
the AppImage, `~/.local/share/bbport/mods/`. Unpack a downloaded mod like this:

```text
mods/
  Mod name/
    dvdroot_ps4/
      chr/...
      param/...
      menu/...
```

Also accepted:

- `Mod name/app0/dvdroot_ps4/...` and `Mod name/CUSA03173/dvdroot_ps4/...` (any of Bloodborne's
  retail title IDs);
- one extra wrapper folder, as left by extracting an archive into a folder of its name:
  `Mod name/Mod name v1.2/dvdroot_ps4/...` (readme files and pictures may sit next to it);
- a mod without `dvdroot_ps4` that holds the game's folders directly: `Mod name/chr/...`,
  `Mod name/parts/...` (`chr`, `parts`, `map`, `menu`, `msg`, `param`, `sfx`, `sound`, `font` and
  the other folders of `dvdroot_ps4`).

Case in a mod's names does not matter: `DVDROOT_PS4/Chr/C0000.chrbnd.dcx` from a mod built on
Windows replaces the game's `dvdroot_ps4/chr/c0000.chrbnd.dcx`. Earlier such a file was added next
to it under its own name, and the game did not see it. The launch log shows how many game files
the mods replaced and how many they added: `Mods: 12 game files replaced, 0 added`. If a mod is
meant to replace files and reports `0 game files replaced`, check the paths inside it. ZIP/7z
archives must be unpacked first. Supported: ready-made replacements of `dvdroot_ps4` files
(textures, models, parameters, interface, sound and fonts). The manager does not merge the
contents of two copies of the same `.dcx`/`.bnd` file: the last replacement wins whole.

For a folder already prepared for shadPS4, a sibling folder `CUSA03173-mods/dvdroot_ps4/...` next
to `CUSA03173/` works. It is added automatically before the mods in the list; the master switch
turns it off too. A folder that directly contains `dvdroot_ps4` can also be chosen as the mods
folder; it is then added whole, without a list of individual mods. The `-mods` convention was
checked against [shadPS4's sources](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/file_sys/fs.cpp).

The game's original files are never overwritten. For the launch, `scripts/mods.py` builds a
separate merged tree of links in the data directory, `out/mod-game-*`; reads, stat and directory
listings see the selected replacements and the remaining original files. The game's `/app0` and
`/hostapp` are read-only. Saves stay in the same folder and remain writable. On a normal exit or a
stop from the launcher the temporary tree is removed. After SIGKILL or a power loss it may remain;
such folders are not used by the next launch.

Mods that replace `eboot.bin`, `sce_module` or `sce_sys`, DLL injectors and script plugins are not
supported by this loader. Patches to executable code stay in the existing `patches/` /
`BB_PATCHES` system. Symbolic links inside a mod are not accepted. Whether a mod works with
CUSA03173 1.09 is up to its author.

From a terminal:

```sh
BB_GAME_DIR=/path/to/CUSA03173 BB_MODS_DIR=/path/to/mods bash run.sh
BB_GAME_DIR=/path/to/CUSA03173 BB_MODS_ENABLED=0 bash run.sh
```

Order and disabled mods are kept in the data directory's `mods.json`:

```json
{"order": ["First", "Second"], "disabled": ["First"]}
```

New folders are enabled automatically and appended in alphabetical order. `BB_MODS_CONFIG` changes
the profile's path. `BB_MOD_TRACE=1` with `BB_MODS_DIR` logs the first reads of files that actually
come from a mod, for diagnostics.

## Third-party patches (shadPS4 XML)

The `patches/` folder in the data directory (in the AppImage, `~/.local/share/bbport/patches/`;
in the launcher, the **Third-party patches** group, where another folder can be chosen). Patch XML
files in the shadPS4/GoldHEN format are accepted: `Metadata` for `AppVer="01.09"` and `eboot.bin`
is used, files with the `TitleID` of other games are skipped. Whether a patch is on by default
comes from the file's `isEnabled`; the launcher's choice is saved in the data directory's
`patches.json`:

```json
{"enabled": ["file.xml/Patch name"], "disabled": ["file.xml/Other patch"]}
```

Supported lines: `bytes`, `bytes16/32/64`, `float32/64`, `utf8`, `utf16`. A patch with `mask`
lines (pattern search) or with an address outside the eboot is skipped whole, with a message in
the log. Third-party patches are applied after the built-in ones (`patches/Bloodborne.xml`): where
they overlap, the third-party one wins. From a terminal:
`BB_PATCHES_DIR=/path BB_PATCHES_CONFIG=/path/patches.json bash run.sh`.

Patches that replace pointers in tables (such as `60 FPS++` and `90 FPS++`) are moved by the loader
to the image's real address. Earlier such patches stopped the launch with
`patch overlaps a relocation`.

## TAA sharpening

The `sharpen` and `sharpness` settings also apply to TAA, in the launcher and the in-game menu. The
range is **0…1**; at 0 or with the switch off there is no extra pass. RCAS is applied to the
finished scene before the HUD. TAA history keeps the colour from before sharpening, so that
sharpening does not build up through repeated accumulation. Flat areas keep their original colour,
HDR included; no extra buffer is needed.
