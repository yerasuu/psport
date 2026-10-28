# Ngs2 custom racks (kb #1098)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F6 of kb #625) does with the custom racks
`0x4001`, `0x4002` and `0x4003`, written from firmware 11.00's `libSceNgs2.sprx` as rea read it (rea-ngs2
spec 02 §4.5, 03 §5.5 and §6, 04 §2.2, §2.5, §2.6 and §4.3, 06, 07 §6, 09 §3 and §12): the module list and
module option checks, `sceNgs2CustomRackGetModuleInfo`, the module parameters and their read-back, and the
PCM of chains of filter, gain, compressor, peak meter, LFE filter, reverb and the game's own UserFx and
UserFx2 handlers, so that the console, the firmware module run by psport and the native one can be
compared sample by sample.

It writes to `/data/psport-hwtest/ngs2_custom/`:

- `results.csv`, one row per check: `case,value,expected,match`. `expected` is what the spec predicts and
  `match` whether the run agreed (`ok` or `MISMATCH`); a row with no `expected` is something the spec
  leaves open, which the console's value settles.
- `<case>.csv`, the rendered PCM of each chain: `frame,channel 0,channel 1,...` as F32, 4096 frames, and
  `<case>_pass.csv`, the same chain with its modules left without parameters, which pass the input through:
  the reference each chain is compared with.

Every chain plays an F32 input (two sines whose amplitude rises over the 4096 frames) from a sampler voice
into a custom mastering voice (the module list under test, output to buffer 0) and a render buffer of as
many channels.

| Case | What it does | What the spec predicts |
|---|---|---|
| `option_*` | `sceNgs2RackQueryBufferSize` of custom racks with broken module lists and module options | `0x804A0A06` stateSize 3, `0x804A0A02` 9 buffers, `0x804A0A01` 25 modules, `0x804A0A00` module ids 0x21 and 0x80 (0x70 accepted), `0x804A0A03` buffer ids past numBuffers, `0x804A0A04` a state offset under the rack's own state (0xc, 0x30 for the custom sampler) or past stateSize, `0x804A0A05` a state past stateSize, `0x804A0A00` a port's buffer past numBuffers; peak meter blocks 1..256 (`0x804A0156`), reverb option of 0xc bytes (`0x804A0081`), delay type, taps and length (`0x804A0532`, `0x531`, `0x530`), pitch shift quality (`0x804A0540`), UserFx2 option of 0x48 bytes. **Unconfirmed**: the envelope's and peak meter's option sizes (0xc), the codes of the envelope's (`0x804A0107`) and chorus' (`0x804A0520`) maxima, and that module options come after the rack's common checks (`option_order_common_first`). The buffer sizes are open |
| `info_*` | `sceNgs2CustomRackGetModuleInfo` | `0x804A0054` under 0x20 bytes, `0x804A0B00` past the list, the module's id, buffers and state |
| `param_*` | ids, sizes and ranges of module parameters | `0x804A0308` for a module id that is not the list's at that index or an index past it, `0x804A030A` a wrong size, `0x804A0603` an LFE cut-off of 100 Hz, gain clamped to [-32, 32], the filter's type and mask kept as bytes. **Unconfirmed**: an unset gain reads 1.0 and an unset LFE {0, 120} (spec 03 §6.8's defaults) |
| `module_read_*` | `0xc001` | the pending blocks in the firmware's layout (spec 09 §12): gain's 8 floats, the filter's location, type and mask bytes, an unset module as zeros, the custom mastering's output module after the list |
| `gain*` | gain {0.5, -2}; then into buffer 1, copied back to buffer 0 by a module without a parameter | `x * {0.5, -2}`: the routing of 0x12a90 |
| `chain_filter_gain_user_fx`, `user_fx_*` | low-pass 1 kHz Q 0.707, gain 0.5, a UserFx that halves the grain | the RBJ low-pass of spec 06 §4 times 0.25; the handler called once per grain with 2 channels, the grain, the rate and its user data. **Unconfirmed**: its flags (3 on its first grain, 0 after); a handler answering < 0 leaves the voice `ERROR` without `PLAYING` (0x11) |
| `compressor`, `state_*` | compressor (threshold 0.2, slope 0.25, attack 1 ms, release 50 ms) and a peak meter of 4 blocks | the PCM and the state values are open: the voice state holds the compressor's {level, gain} at its offset and the meter's {peak, channel peaks} at its own |
| `lfe`, `lfe_routed` | the LFE filter at 120 Hz on 6 channels, in place and into another buffer | channel 3 low-passed; **unconfirmed**: with source and destination apart the second stage reads the source again (spec 06 §5), so only the second section's response reaches channel 3 |
| `reverb_*` | module `0x1b` (option word 1, the 14400-sample line) against the reverb rack `0x2001` with the same room | the same PCM: the module is the rack's network |
| `user_fx2_*` | UserFx2 with setup, cleanup, control and process handlers | `0x804A0207` without data, `0x804A0209` a copy of the wrong size without a control handler; the control handler's parameter reaches the process. **Unconfirmed**: setup once per voice when the rack is created and cleanup when it is destroyed, the process context's channels, user data, flags and state pointer |
| `custom_sampler_*` | a custom sampler: setup, blocks and pitch through `0x40010000`, `0x40010001` and `0x40010005`, gain 0.5 into buffer 1, port 0 from buffer 1 | `x * 0.5`; `0x804A0308` for `0x40018005`. **Unconfirmed**: the source's data in the voice state from +0x10 on (+0x08 open) |

psport's host check compares the native modules with references written from the spec (filter types,
compressor, LFE from 06-tablas §B, peak meter) to float rounding; what this test adds is the console's
answer to what rea marked as inferred or did not read: the descriptors' buffer flags, UserFx2's life
cycle, the module info callbacks.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_custom
```

Install `IV0000-PSPT01098_00-NGS2CUSTOM000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one psport
follows), start "Ngs2 custom rack test", wait for it to exit (the klog shows `ngs2_custom: done`), and
copy `/data/psport-hwtest/ngs2_custom/` off the console over FTP. Run the same build through psport as a
title folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and once with the firmware module
(`PS_LLE=libSceNgs2`), and diff the three folders: `results.csv` should match, and the PCM files should
agree to float rounding.
