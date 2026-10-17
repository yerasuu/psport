# Emulated LEGO Dimensions toy pad

psport can plug in a virtual LEGO Dimensions toy pad, the USB portal figures stand on
(`src/system/runtime_toypad.c`, ported from bbport `72e5c8c`). It is a generic peripheral behind
libSceUsbd, offered to any game that looks for it, and it is **off by default**: without it
libSceUsbd lists no USB device, as a console with nothing plugged in does.

| Setting | Effect |
|---|---|
| `PS_TOYPAD=1`, else `toypad=1` in `psport.ini` | plugs the pad in; `PS_TOYPAD=0` overrides the file |
| `PS_TOYPAD_FILE` | the figures file, instead of `<user>/toypad/figures.txt` |
| `PS_TOYPAD_NAMES` | the figure names file, instead of `figure_ids.txt` next to the figures file |

`<user>` is the folder saves live in: `PS_USER_DIR`, else `user/` in the data directory (`PS_DATA_DIR`).

## What the game sees

The device is a USB HID device, vendor `0x0E6F`, product `0x0241`, with one interrupt endpoint
each way and 32-byte reports. `sceUsbdGetDeviceList` returns it, `sceUsbdOpenDeviceWithVidPid` and
`sceUsbdOpen` open it, and its transfers complete through `sceUsbdHandleEvents*` with libusb's
semantics: OUT requests at once, IN transfers when a reply or a figure report is waiting, or at
their timeout. The pad answers the wake, seed and challenge commands (TEA with the pad's key),
figure reads (`0xD2`), writes (`0xD3`) and model queries (`0xD4`), and records the lights the game
sets (`0xC0`-`0xC8`). Other commands get an empty acknowledgement, logged and counted in the call
report (`toy pad command answered empty`).

## Figures

Nothing copyrighted ships with psport: figures are made from their ids.

- `<user>/toypad/figures.txt` lists what stands on the pad, one `<zone> <figure id>` per line,
  `#` starting a comment. Zone 1 is the centre (one figure), 2 the left and 3 the right (three
  each), seven figures at most. The file is read again whenever it changes, so figures can be put
  on and lifted off while the game runs; the same id twice is two copies of that figure.
- `<user>/toypad/<id>-<copy>.bin` is each figure's tag, a 180-byte NTAG213 image (45 pages): made
  the first time the figure is placed (a character's id encrypted with its tag's key, vehicles and
  gadgets, ids from 1000, in plain) and written as the game writes the tag (vehicle upgrades). A
  dump of a real tag in the same format can replace it.
- `<user>/toypad/figure_ids.txt` (optional) names the figures for the overlay, one `<id> <name>`
  per line; without it the overlay shows the ids.

## Overlay

Once the game has initialized libSceUsbd with the pad connected, the overlay (Insert or L3+R3) shows
a **Toy pad** section: choose a figure, press **+ put here** on a zone, press a placed figure to
lift it. The dot before each zone is its light as the game set it: when the game asks for a figure
it lights the zone to put it on. The section writes `figures.txt`, so the runtime picks the change
up as if the file had been edited by hand.

## Log

`Runtime: Usbd initialized; the emulated toy pad is connected (figures from ...)` at start;
`Runtime: toypad: figure <id> placed on zone <z> (reader index <n>)` and `... lifted from zone <z>`
as figures change; `Runtime: toypad commands=..., figure reports=..., dropped=...` in the exit
report.
