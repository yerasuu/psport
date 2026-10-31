# Games

What each title does under psport. The [README](../../README.md) lists only the state; this page has the detail.
Other languages: [Español](../es/GAMES.md), [Русский](../ru/GAMES.md).

## States

| State | Meaning |
|---|---|
| playable | The title can be played: controls, saves where the game has them, and sound work for everything seen so far. |
| experimental | It runs and shows menus or gameplay, but only a limited part has been seen or only a scripted run covers it; problems are likely. |
| broken | It starts, but a core part fails (a black scene, a crash). |
| not supported | It cannot work with psport as it is (for example, it needs hardware psport does not provide). |

A state is how far a title has been seen to get, not a full play-through. Every title is listed in
[tools/regress/titles.txt](../../tools/regress/titles.txt) and run headless with `tools/regress.sh`
(see Testing in the README); "regress" below means what that scripted run records: the first GPU
frame, frames drawn, sound. The detail was last checked against the regress of 2026-10-22.

## Titles

| Title | Title ID | State | Detail |
|---|---|---|---|
| Bloodborne (1.09) | CUSA03173 | playable | Saves, several areas, sound, pad, keyboard and mouse (the key prompts are drawn over the pad's), frame-rate presets 30, 60, 90 and uncapped, 3840x2160 output (about 80 FPS in the test scene on an RX 7900 XT). Online play through the title's community servers (`PS_ONLINE=1`) works, with occasional stutters when interacting with messages. Per-title fixes are declared in [QUIRKS.md](QUIRKS.md). |
| Atelier Ryza | CUSA16458 | playable | Title menu, prologue, the field with the player in control, sound; the river and the forest draw correctly. Saving and loading work. |
| Atelier Ayesha DX | CUSA16921 | playable | Intro movie, title menu, opening scenes, the field, the atelier, sound; the key prompts are drawn for keyboard play. |
| Tales of Symphonia Remastered | CUSA50989 | experimental | Title screen, the first room, sound. |
| Astrea | CUSA49515 | experimental | The intro's story panels, sound. |
| SaGa Emerald Beyond | CUSA43027 | experimental | Title screen, first story scenes with dialogue, sound. |
| ALTDEUS: Beyond Chronos | CUSA26563 | not supported | Black screen: it requires a PS VR headset and has no flat-screen mode. |
| Gal Guardians: Demon Purge | CUSA41240 | experimental | Regress: frames drawn, sound. |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | experimental | Regress: frames drawn, sound. |
| Call of Duty: Black Ops | CUSA57547 | experimental | Regress: frames drawn, sound. Keyboard and mouse with the game's own camera, a shooter layout and a crosshair. |
| Call of Duty: Black Ops II | CUSA57548 | playable | First mission (Pyrrhic Victory) through chained checkpoints to the river barge, Save and Quit, Resume Story, videos, sound, pad. |
| Blood Omen 2 | CUSA49769 | experimental | Regress: frames drawn, no sound. |
| Onigiri | CUSA01458 | experimental | Regress: frames drawn, sound. |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | broken | Loads a saved game and draws frames and sound, but the 3D scene stays black (only the minimap shows). Left out of routine runs. |

## Notes

- Titles that require PS VR (`require_ps_vr` in `param.sfo`) are not supported.
- No tested title needs a dump of the console's system modules.
