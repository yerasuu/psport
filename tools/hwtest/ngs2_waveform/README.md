# Ngs2 waveforms (kb #1096, #1119)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F4 of kb #625) does with waveforms:
the parse functions on synthetic files, the frame shapes and blocks of each format, and the VAG,
generator and ATRAC9 sources rendered, so that the console, the firmware module run by psport and the
native one can be compared.

The `expected` values of the parse, frame, block and pool rows are what rea read in firmware 11.00's
parse functions, `sceNgs2CalcWaveformBlock`, `sceNgs2GetWaveformFrameInfo` and PLAY (spec 10, kb
#1119); the render rows follow spec 05 §4 to §6 and spec 10 §3, §4. Inputs on which the firmware
divides by zero (PCM with 0 channels and no `fact`, an ATRAC9 config without frames given to a parse or
to `sceNgs2CalcWaveformBlock`) are left out, since they would stop the console.

It writes to `/data/psport-hwtest/ngs2_waveform/`:

- `results.csv`, one row per check: `case,value,expected,match`. A row without `expected` is open:
  nothing predicts it, the console's value settles it.
- `<case>.csv`, the rendered PCM of each render case: `frame,channel 0,channel 1,...` as F32.

| Case | What it does | What psport predicts |
|---|---|---|
| `parse_pcm16`, `parse_u8` ... `parse_ext_f32` | RIFF WAVE, 16-bit stereo 44.1 kHz with a `smpl` loop at 100..899 of 1000 samples; then 8 (unsigned), 24, 32-bit PCM, 32 and 64-bit float, and WAVE_FORMAT_EXTENSIBLE | types `0x12`, `0x11`, `0x14`, `0x16`, `0x18`, `0x1a`; the loop 100..900 (end exclusive) in three blocks, the loop endless; IEEE float extensible `0x804A0401` (the firmware has no subformat for it) |
| `parse_at9` | an `.at9` header: config `FE 74 0D F0`, fact {11000, 256, 256}, a `smpl` loop at 1256..10255 | delay 256 (one frame), the loop moved back by it to 1000..10000, frames of 112 bytes and 256 samples, superframes of 4 (448 bytes, 1024 samples); blocks {0+896 skip 256}, {0+4928 skip 1256, endless}, {4032+896 skip 1040} past the data offset |
| `parse_at9_file`, `parse_at9_user` | the same file 10 bytes into `/data/.../synthetic.at9`, and through a read handler | the same info as from memory |
| `parse_vag`, `parse_vag_no_loop_bit` | VAGp, mono 22.05 kHz, 10 blocks with flags 6 on block 3 and 3 on block 6, with and without the loop bit (2) on block 1 | with it: loop 84..196 in three blocks by whole frames, configData 0; without it: no loop, 252 samples (the last frame left out), one block |
| `parse_error_*` | truncated, null, missing and unknown input, unchecked fields | info `0x804A0053` before anything in `Data`, then a size below 4 `0x804A0408` before null data `0x804A0406`; a null handler `0x804A0408` before the info; any failed read `0x804A0408` whatever the handler answers; `0x8002000E` for a file that does not open, info or not; `0x804A0401` for bits and tags; 9 channels, 0 Hz, a data chunk past the bytes given, a form other than `WAVE` and a config without 0xFE parse; an odd chunk, unpadded, hides the data (`0x804A0408`); ATRAC9 without `fact` `0x804A0408`; on failure only the info's type and channels are cleared |
| `frame_*`, `calc_*` | `sceNgs2GetWaveformFrameInfo` and `sceNgs2CalcWaveformBlock` on PCM, VAG (with a frame margin) and ATRAC9, and invalid formats | outputs cleared first; `0x804A0053` null block, `0x804A0400` null format, `0x804A0401` other types and generators, nothing else checked; frameMargin left out; VAG by whole frames with no skip; ATRAC9 from the superframe of the first sample with skip `i mod 1024 + 256` (1000 samples in: offset 0, skip 1256); 32-bit wrap |
| `vag_loop`, `vag_flags`, `vag_plain` | the VAGp above at 48 kHz: its three parsed blocks with an exit request; one block with configData bit 0 and an exit request; one block without it | the first frames match a decoding of spec 05 §4 from output 3; frames 3 to 6 loop until the exit request, then the voice ends; plain, all 280 samples play (278 non-zero) |
| `gen_*` | the generators `0x80` to `0x87`, and the sine at pitch 2 | outputs 0 to 2 silent (the resampler's history), the sine within 1e-3 of 1 kHz from 90 degrees and of 2 kHz at pitch 2; white noise's first sample 2^-31 at output 3, the periodic register 0 there; the other first samples open |
| `pool_*` | the synthetic `.at9`'s format set up on two voices: one internal decoder; then two decoders with `maxAtrac9ChannelWorks` 3 | the second PLAY `0x804A0307`, and after KILL of the first, 0; two stereo voices need 4 channel works, `0x804A0307` |
| `at9`, `at9_ajm`, `at9_*` | with `input.at9` in the output folder: the file parsed by path and played for 400 grains with the internal decoders, then the AJM ones; the setup's ATRAC9 checks | the first non-zero output of each (open: the firmware's AJM path runs a grain behind, psport's does not); `0x804A0402` without decoders of the kind asked, `0x804A0403` without the 0xFE byte, `0x804A0052` for other channels; a sample rate index the firmware's frame table leaves out sets `ERROR` |

For the ATRAC9 cases, put any `.at9` in `/data/psport-hwtest/ngs2_waveform/input.at9` first, for
instance a game's `sce_sys/snd0.at9`; without one those rows are skipped (`at9_input_present` 0).

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_waveform
```

The toolchain's `libSceNgs2` stub must export the five waveform functions
(`sceNgs2ParseWaveformData`, `File`, `User`, `sceNgs2CalcWaveformBlock`,
`sceNgs2GetWaveformFrameInfo`); if the link misses them, add their NIDs to the stub.

Install `IV0000-PSPT01096_00-NGS2WAVEFORM0000.pkg` on a jailbroken PS4 (firmware 11.00 is the one
psport follows), start "Ngs2 waveform test", wait for it to exit (the klog shows `ngs2_waveform:
done`), and copy `/data/psport-hwtest/ngs2_waveform/` off the console over FTP. Run the same build
through psport as a title folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and once
with the firmware module (`PS_LLE=libSceNgs2`), and diff the three folders: `results.csv` should
match, and the PCM files should agree to float rounding (ATRAC9 to LibAtrac9's against the firmware's
own decoder, which is not bit-exact).
