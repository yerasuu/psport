# Ngs2 waveforms (kb #1096)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F4 of kb #625) does with waveforms:
the parse functions on synthetic files, the frame shapes and blocks of each format, and the VAG,
generator and ATRAC9 sources rendered, so that the console, the firmware module run by psport and the
native one can be compared.

rea has not read the parse functions yet (spec 08 §4). Their `expected` values below are psport's
reading of RIFF WAVE, `.at9`, VAGp and the SDK's `SceNgs2WaveformInfo`, not the spec's; a mismatch
on the console is what psport learns. The render rows follow spec 05 §4 to §6.

It writes to `/data/psport-hwtest/ngs2_waveform/`:

- `results.csv`, one row per check: `case,value,expected,match`. A row without `expected` is open:
  nothing predicts it, the console's value settles it.
- `<case>.csv`, the rendered PCM of each render case: `frame,channel 0,channel 1,...` as F32.

| Case | What it does | What psport predicts |
|---|---|---|
| `parse_pcm16`, `parse_u8` ... `parse_ext_f32` | RIFF WAVE, 16-bit stereo 44.1 kHz with a `smpl` loop at 100..899 of 1000 samples; then 8 (unsigned), 24, 32-bit PCM, 32 and 64-bit float, and WAVE_FORMAT_EXTENSIBLE | types `0x12`, `0x11`, `0x14`, `0x16`, `0x18`, `0x1a`; three blocks around the loop, the loop endless; the block fields are open rows |
| `parse_at9` | an `.at9` header: config `FE 74 0D F0`, fact {11000, 256, 256}, a `smpl` loop at 1256..10255 | delay 256, the loop moved back by it to 1000..9999 (as the `.at9` files of Ryza and Saga need), frames of 112 bytes and 256 samples, superframes of 4 (448 bytes, 1024 samples) |
| `parse_at9_file`, `parse_at9_user` | the same file 10 bytes into `/data/.../synthetic.at9`, and through a read handler | the same info as from memory |
| `parse_vag` | VAGp, mono 22.05 kHz, 10 blocks with flags 6 on block 3 and 3 on block 6 | configData 1 (the sampler follows the flags), loop 84..195, data at 0x30 |
| `parse_error_*` | truncated, missing, unknown and invalid input | open: psport answers `0x804A0407` (size), `0x804A0408` (chunk), `0x804A0401` (unknown file or tag), `0x804A0400` (bits), the setup's codes for channels, rate and ATRAC9 config, `0x804A0053`/`0x804A0406` for null addresses, the read handler's own error, `0x80020002` for a missing file |
| `frame_*`, `calc_*` | `sceNgs2GetWaveformFrameInfo` and `sceNgs2CalcWaveformBlock` on PCM, VAG (with a frame margin) and ATRAC9, and invalid formats | frames as the sampler reads them (spec 05 §2); an ATRAC9 delay of one frame (256); a block from the frame holding the first sample after the delay (1000 samples in: superframe 1, skip 232); the setup's codes, in its order |
| `vag_loop`, `vag_loop_exit`, `vag_plain` | the VAGp above at 48 kHz, its flags followed, then an exit request; then without configData bit 0 | the first frames match a decoding of spec 05 §4 from output 3; frames 3 to 6 loop until the exit request ends the block at the loop end; plain, all 280 samples play (278 non-zero) |
| `gen_*` | the generators `0x80` to `0x87` (spec 05 §5) | sine at 1 kHz from 90 degrees within 1e-3; the first samples of each, noise included, are open rows (where the firmware seeds its noise registers is not read) |
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
