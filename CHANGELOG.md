# Changelog

What each psport version brings, grouped by area, for players and launcher authors. The technical
record of a version, terse and anchored in code, is `tag-changelog/vX.Y.Z.md`: it is the message of
the annotated, signed tag `vX.Y.Z`, which the release workflow (`.github/workflows/nix-cache.yml`)
publishes as the GitHub Release's notes. How a release is made: [FORK.md, CI and binary
cache](FORK.md#ci-and-binary-cache).

Versions follow [SemVer](https://semver.org) as it reads below 1.0: in `0.y.z` a breaking change
bumps `y`, a feature, fix or performance change bumps `z`, and documentation, refactoring or build
changes alone make no release. What breaks is psport's launch interface: `run.sh` variables
(`PS_*`), `psport.ini` keys, the launcher queries (`psport --list-*`, `--check-game`,
`--has-presets`, `--online-overrides`) and their output, and the on-disk layout of the data folder
(`user/` saves, trophies and caches, `out/`). Between releases `VERSION` reads `0-unstable`.

The tags without a `v` (`0.1` to `0.51`) are bbport's releases, carried in psport's history; their
notes are in [docs/en/history/](docs/en/history/).

## v0.1.0

The first version of psport as an independent project: bbport's native Bloodborne port taken to a
compatibility layer for PS4 titles in general, on shadPS4's video core, with bbport 0.6-pre1's
fixes and features ported by hand.

### Launching
- `run.sh` and `out/psport` launch any dump (`PS_PERMISSIVE=1` for titles other than Bloodborne);
  data, saves, shader caches and `psport.ini` in `PS_DATA_DIR`.
- Launcher queries print JSON: `--list-patches`, `--list-mods`, `--list-quirks`, `--list-traits`,
  `--list-effects`, `--list-features`, plus `--check-game`, `--has-presets`, `--online-overrides`.
- `BB_*` variables and `bbport.ini` from bbport still work for a transition, with a deprecation line.

### Loader and system libraries
- The game's eboot and modules load straight from its folder; mods, XML patches and middleware
  detection (Unity, Mono, Unreal, FMOD, Wwise, Havok).
- System libraries are implemented natively; no tested title needs a dump of system modules
  (`PS_SYS_MODULES` stays optional, for comparison).
- Trophies kept per user; system fonts from a dump or the host's Noto fonts.
- Every unimplemented, stubbed or failing call is logged and counted in the exit call report.

### Graphics
- Cross-vendor Vulkan 1.3; the game's memory used in place on every GPU (bbport's 0.3 memory model
  and its variables are gone).
- Malformed command buffers, untranslatable shaders and unsupported formats are skipped and
  counted instead of stopping the game.
- Upscalers FSR 3.1, FSR 4, FSR 4.1.1, native TAA and DLSS where a game profile places them;
  output aspect ratios from 4:3 to 32:9.
- Occlusion queries counted for real by default (Bloodborne +8 %).
- The pipeline cache is written atomically and kept across exits.

### Input
- Key and button bindings with chords; a binding for the port's menu (`key.menu`, `pad.menu`).
- Real gyroscope and accelerometer of DualShock 4, DualSense, Switch Pro and Steam Deck; motion and
  touchpad emulation for other controllers (`pad_motion=1`).
- Host mouse and keyboard for games that use them.
- Emulated LEGO Dimensions toy pad (`toypad=1`), off by default.

### Bloodborne
- Frame rate by hooks up to 480 FPS, mouse camera and menu pointer, live aspect ratio, button
  prompts for the keyboard or gamepad in use.

### Online
- Seamless Hunt's shadNet server by default; the client reports its psport version.
- P2P relay fallback; Bloodborne seamless co-op (BBSC) as an opt-in switch with a local game
  server.

### Platform and tools
- `ps_host_*` platform layer with a capability table (`--list-features`); Linux implemented.
- Declared quirks per host or title, listed and disableable (`PS_QUIRKS`).
- Releases ship a relocatable bundle and a Nix binary cache; `tools/regress.sh`, the night script
  and Tracy/perf/RenderDoc profiling for development.
