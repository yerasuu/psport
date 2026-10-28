# Ngs2 pan, geom and FFT (kb #1099)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F7 of kb #625) computes for
`sceNgs2PanInit`, `sceNgs2PanGetVolumeMatrix`, the four `sceNgs2Geom*` and the three `sceNgs2Fft*`,
written from firmware 11.00's `libSceNgs2.sprx` as rea read it (rea-ngs2 spec 12): their checks and error
codes, the pan's volume matrices, the geom's listener frames and source attributes, and the FFT of known
signals, so that the console, the firmware module run by psport and the native one can be compared word
for word.

It writes to `/data/psport-hwtest/ngs2_pan/`:

- `results.csv`, one row per check: `case,value,expected,match`. `expected` is what the spec predicts
  and `match` whether the run agreed (`ok` or `MISMATCH`).
- `pan.csv`, `geom.csv` and `fft.csv`: `case,answer,word,word,...`, each output as its IEEE-754 words
  (the pan work, the volume matrix of each layout, unit, matrix format, distance and angle; the listener
  works and the attribute of 240 sources; the FFT of six signals in each size and order).

| Case | What it does | What the spec predicts |
|---|---|---|
| `pan_init_*` | `sceNgs2PanInit` with null work, a unit of 100, 3 speakers, the default layouts, angles past the circle | `0x804A0053`, `0x804A0910`, `0x804A0911`; 0 speakers is the 7-speaker layout; a default layout takes any unit; custom stereo {330, 30} is kept as {30, 330, 390}; -30 and 750 wrap to 330 and 30. **Unconfirmed**: with 2π the default layout's closing sentinel stays in degrees (450 for stereo), and 0 speakers with 2π converts nothing |
| `pan_matrix_*` | `sceNgs2PanGetVolumeMatrix` checks, stereo at 30 degrees, stereo spread | `0x804A0913`, `0914`, `0053`, `0912` (format 3), `0052` (9 parameters); 0.5 and 0.866 between R at 90 and L at 270; 0.7071 each with distance 0 |
| `geom_reset_*` | the two resets | speed 340.29, front (0, 0, -1), reserved words untouched; rolloff model 5, outer level 0.2, 7 speakers, format 8 |
| `geom_listener_*` | `sceNgs2GeomCalcListener` checks and the reset listener | `0x804A0921`, `0053`, `0923` (coordinate 2); the identity frame 0.17 % short of 1 (the firmware's fast inverse square root) |
| `geom_apply_*` | `sceNgs2GeomApply` checks, a source 2 m ahead, a listener moving toward it | `0x804A0921`, `0922`, `0053`, `0923` (flags 0, 0x14, 0x20), `0920` (model 6, reference past max with model 3), `0924` (inner angle 400, inner past outer); volume 0.5 and A3D z 2 under the inverse model; pitch (340.29 + 5) / 340.29 |
| `fft_size_*`, `fft_init_*` | sizes for 2^7 to 2^13 points | -1 outside 2^8..2^12; 4N + 64 bytes real, 8N + 64 complex; -1 for a buffer one byte short |
| `fft_<n>_*` | an impulse, cos at bin 3, sin at bin 5, the Nyquist alternation (scaled), a round trip (unordered) | a flat spectrum; N/2 at `out[6]`; -N/2 at `out[11]`; 1 at `out[1]`; the input back |
| `fft_complex_*` | **Unconfirmed**: the complex mode (Init without flag 2) | firmware 11.00 transforms nothing: forward, unordered, 256 points returns the work buffer as it was |

The rows of `pan.csv` and `geom.csv` should match the console's word for word: psport's host check runs
the firmware's own code for these functions on the host (glibc's `sinf`, `acosf`, `powf`, `sincosf`) and
the native code matches it bit for bit, except the sign of a NaN. A difference in the last bits there is
the console's libm. `fft.csv` matches to float rounding only (psport computes the same DFT another way:
about 2.5e-7 of the peak on the host); its complex rows should match exactly.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_pan
```

Install `IV0000-PSPT01099_00-NGS2PAN000000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one psport
follows), start "Ngs2 pan test", wait for it to exit (the klog shows `ngs2_pan: done`), and copy
`/data/psport-hwtest/ngs2_pan/` off the console over FTP. Run the same build through psport as a title
folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and once with the firmware module
(`PS_LLE=libSceNgs2`), and diff the three folders.
