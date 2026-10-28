# Ngs2 reverb (kb #1097, kb #1121)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F5 of kb #625) renders for the reverb
rack `0x2001` and its module `0x1b`, written from firmware 11.00's `libSceNgs2.sprx` as rea read it
(rea-ngs2 spec 07 and 06-tablas §E): the rack's option and parameter checks, the I3DL2 read-back, and
the impulse response of a set of test rooms, so that the console, the firmware module run by psport
and the native one can be compared sample by sample.

It writes to `/data/psport-hwtest/ngs2_reverb/`:

- `results.csv`, one row per check: `case,value,expected,match`. `expected` is what the spec predicts
  and `match` whether the run agreed (`ok` or `MISMATCH`); a row with no `expected` is something the
  spec leaves open, which the console's value settles.
- `<room>.csv`, the rendered PCM of each impulse response: `frame,left,right` as F32, 48128 frames.

Every response plays a mono F32 impulse of 0.25 from a sampler voice through a reverb voice (1 channel
in, 2 out, wet 1 and dry 0 but for `sparse`) into a 2-channel mastering voice; the sampler's 4 samples
of history put the impulse at frame 3.

| Case | What it does | What the spec predicts |
|---|---|---|
| `option_*` | `sceNgs2RackQueryBufferSize` of the reverb rack with valid and invalid options | `0x804A0900` for reverbSize 2, `0x804A0103` for 0 or 17 voices, `0x804A0104` for 0 or 9 channels; reverbSize 1 takes 9600 floats more than 0 (a reflection line of 14400 samples against 4800). The sizes themselves are open: psport's layout is not the firmware's |
| `setup_*`, `unknown_id`, `read_setup*` | setups of 3 and 0 channels, a short parameter, id `0x20010003`, the setup's read | `0x804A0052`, `0x804A030A`, `0x804A0308`; 1 in and 2 out |
| `defaults_*` | the I3DL2 read right after the setup | the defaults at `0x335a8`; **unconfirmed**: `reverb` reads -2147481248 (log10 of the stored 0 truncated as x86 does, plus 2400) |
| `clamp_*` | out-of-range I3DL2 values | the setter's clamps (spec 07 §2.1); `clamp_wet` (40 asked) is open: 32 from SDK 6.00 on, 1 before |
| `round_trip_*` | each test room set and read back | the same millibels (room above 0 reads 0) |
| `explicit_*` | the explicit network `0x20010002`: a tap past the 14400-sample line, combs one slot over their memory, combs filling it, an all-pass over its memory | `0x804A030A`, `0x804A030A`, 0, `0x804A030A`; **unconfirmed**: the I3DL2 read then answers -1 |
| `ir_<room>_first_sample` | the first non-zero frame of each response | 3 + reflectionsDelay x 48000: nothing reaches the output before tap 0 |
| `ir_<room>_energy`, `ir_<room>_state` | the response's energy, the reverb voice's state | open; flags 3 (in use, playing) |
| `ir_bright_first_tap_*` | room and reflections at 0 mB, no delay, no high-frequency filter | tap 0 at once with the pattern's 0.7: 0.175 on both channels |
| `explicit_tap_*` | **unconfirmed**: an I3DL2 grain (room and reflections 0 mB, reverb -10000 mB), then one explicit tap of 10.1 ms and gain 0.5 with no comb bits; the I3DL2 grain comes first because an explicit network without all its comb bits right after a reset hangs the firmware by rea's reading | 0.125 at frame 3 + align8(485) = 491, tap 0 routed as an early tap with the early group gain of 1 the I3DL2 grain left |
| `module_read_*` | `0xc001` on the reverb voice after the concert hall room | the 0x150-byte block: mode 0, wet 1, room 0.3162, decayTime 3.92, reverb at -24.02 dB, HFReference 5000, the dry and wet levels at 0xd0 and 0xf0 (wet of channel 3 at 0), bit 3 of +0x132 set (`module_read_bypass_mask`); the other words are open |
| `explicit_after_reset_*` | **unconfirmed**: the explicit network right after a reset, the 16 combs and 4 all-passes set, only the late tap | rea's reading (spec 11 §6.4): the reset leaves the prefilter's table zeroed and the four-sample comb path feeds the combs 0, so the response is silent (`explicit_after_reset_silent` 1). psport's native run rings (0) on purpose until this row is answered: it passes the late tap on as the one-sample path does (`FORK.md`) |
| `explicit_partial_mask_*` | **RISK, runs last**: the explicit network right after a reset with only comb 0 set | rea's reading: the 15 combs of length 0 make `0x27fc0` loop forever, so the console hangs during the render. `explicit_partial_mask_start` is flushed before it; a `results.csv` that ends there means it hung (reboot the console), `explicit_partial_mask_done` means it did not. psport keeps such combs silent and counts them |

The rooms are modeled on the I3DL2 presets (generic, padded cell, bathroom, stone room, concert
hall, cave, arena, hangar, plate) plus a sparse room at the reflection line's longest delays and a
bright one with decayHFRatio 2 and pattern 15. psport's host check compares the native responses
with an independent reference of spec 07 to about 1e-6 of their peak; what this test adds is the
console's answer to the parts of spec 07 rea marked as inferred.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_reverb
```

Install `IV0000-PSPT01097_00-NGS2REVERB000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one
psport follows), start "Ngs2 reverb test", wait for it to exit (the klog shows `ngs2_reverb: done`),
and copy `/data/psport-hwtest/ngs2_reverb/` off the console over FTP. Run the same build through
psport as a title folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and once with the
firmware module (`PS_LLE=libSceNgs2`), and diff the three folders: `results.csv` should match, and the
PCM files should agree to float rounding.

**Risk:** the last case, `explicit_partial_mask_*`, is expected to hang firmware 11.00 (the console, and
psport with `PS_LLE=libSceNgs2`); every other row is written by then. If the klog stops at
`ngs2_reverb: explicit_partial_mask may hang the console` and `ngs2_reverb: done` never comes, reboot
the console (or stop psport after a minute) and keep the folder as it is: its `results.csv` ending at
`explicit_partial_mask_start` is the answer.
