# Ngs2 render (kb #1095)

Checks on a PS4 what psport's native Ngs2 (`gpu/ngs2/`, phase F3 of kb #625) renders from
firmware 11.00's `libSceNgs2.sprx` as rea read it: synthetic waveforms played through sampler,
submixer and mastering voices, each case's output PCM written in full so that the console, the
firmware module run by psport and the native one can be compared sample by sample.

It writes to `/data/psport-hwtest/ngs2_render/`:

- `results.csv`, one row per check: `case,value,expected,match`. `expected` is what the spec
  (rea-ngs2 spec 03 to 06) predicts and `match` whether the run agreed (`ok` or `MISMATCH`); a row
  with no `expected` is something the spec leaves open, which the console's value settles.
- `<case>.csv`, the rendered PCM of each case: `frame,channel 0,channel 1,...` as F32.

| Case | What it plays | What the spec predicts |
|---|---|---|
| `silence` | a playing mastering voice with nothing patched | zeros, flags `IN_USE`, `PLAYING` |
| `dc` | 1024 samples of 0.5, mono, into a 2-channel mastering voice | 0.5 x 0.7071 on both channels (fixed 1->2 port matrix); 1023 samples, the resampler starting on input sample 1 with an empty history |
| `pitch_*` | a ramp n/65536 at pitch 1, 2, 0.5, 3 and from a 24 kHz waveform | (1 + slope x i)/65536: phase 0 taps for 1, 2, 3 and the symmetric phase 256 for 0.5 |
| `loops`, `exit_loop` | block A (0..99) repeated twice then block B; A repeated forever with an exit-loop request | A A A B from A[1]; two loop and two block-end callbacks; B from stream sample 300 |
| `streaming` | a block queued as not last, a second one after a render | 99 samples, starved flags `0x21`, then 200 samples |
| `events`, `envelope` | PAUSE, RESUME, STOP on DC 0.5; an envelope (attack 128, release 384) and STOP | 64-sample fades at the tail, head and head; the release instead of the STOP fade |
| `mastering_8_to_*` | an 8-channel DC rendered to 2, 6 and 8 channels | the default 8->N matrices; channel 3 through the 120 Hz LFE low-pass |
| `limiter`, `gain` | a 0.9 sine through a mastering matrix of 2.0, then gain 0.5 | peak 1.0 after the limiter, then 0.5; meters 0.9 in, 1.0 out; I16 at 0.5 x 32767 |
| `filter_*` | DC 0.5 through a 1 kHz low-pass and high-pass before port 0 | 0.5 and 0 once settled |
| `meters` | -0.3 / 0.6 alternating, sampler meter in mode 1 then 2 | 0.6, then RMS 0.4743 |
| `works_*` | 2-channel voices in a sampler rack of 4 channel works | 0x804A0306 for the third; KILL frees them. PLAY on a voice without setup is open |
| `submixer_delay` | sampler, submixer (port delay 100) and mastering | DC from sample 100 |

The open rows (`works_play_without_setup`, `loops_loop_callback_repeats`, `limiter_state_word_*`,
`meter_decoded_*`) are the questions the spec leaves to the console.

## Build and run

With the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
`OO_PS4_TOOLCHAIN` set to it:

```
make -C tools/hwtest/ngs2_render
```

Install `IV0000-PSPT01095_00-NGS2RENDER000000.pkg` on a jailbroken PS4 (firmware 11.00 is the one
psport follows), start "Ngs2 render test", wait for it to exit (the klog shows `ngs2_render:
done`), and copy `/data/psport-hwtest/ngs2_render/` off the console over FTP. Run the same build
through psport as a title folder (`eboot.bin` and `sce_sys/`), once with `PS_NGS2=native` and once
with the firmware module (`PS_LLE=libSceNgs2`), and diff the three folders: `results.csv` should
match, and the PCM files should agree to float rounding.
