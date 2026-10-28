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
| `dc` | 1024 samples of 0.5, mono, into a 2-channel mastering voice | 0.5 x 0.7071 on both channels (fixed 1->2 port matrix); 1021 samples: the voice starts with 4 zero samples of history (spec 09 §2) and ends with 4 left (spec 05 §3.4) |
| `pitch_*` | a ramp n/65536 at pitch 1, 2, 0.5, 3 and from a 24 kHz waveform | (slope x i - 3)/65536 from output 8: phase 0 taps for 1, 2, 3 and the symmetric phase 256 for 0.5 |
| `loops`, `exit_loop` | block A (0..99) repeated twice then block B; A repeated forever with an exit-loop request | A A A B from output 3; two loop and two block-end callbacks; B from stream sample 300 (output 303) |
| `streaming` | a block queued as not last, a second one after a render | 100 samples, starved flags `0x21`, then 200 samples |
| `blocks_read_*`, `replace_address_*` | **unconfirmed** (spec 09 §6.3): 0x10008001 after 1, 2 and 4 blocks in a ring of 4; 0x10000002 | 4, 1 and 3 records (the walk stops one short of the newest slot and goes round the ring with one), the word at 8 at 0; every slot's address moved |
| `kill_ramp` | KILL on a playing DC 0.5 | the data lock (spec 09 §6.2): the last grain fades the last sample to 0 without reading, under the 64-sample cut, so sample 32 is 0.4375 x 0.5 |
| `module_read_*` | 0xc001 on the sampler's pitch and envelope and the mastering limiter | the firmware's own block layouts (spec 09 §12): pitch 1.0, an unset envelope zeroed, the setup's limiter {1, 1, 0, 0, 0.001, 0.01, 1} |
| `short_block`, `partial_frame` | **unconfirmed** (spec 09 §7): a block of 100 samples of data asking for 150, then block B; the same block two bytes longer | B's data finishing A's region, B dropped with A's userData, the voice ending; a partial frame ending A with `0x21` for the rest of the grain and B from the next |
| `events`, `envelope` | PAUSE, RESUME, STOP on DC 0.5; an envelope (attack 128, release 384) and STOP | 64-sample fades at the tail, head and head; the release instead of the STOP fade |
| `mastering_8_to_*` | an 8-channel DC rendered to 2, 6 and 8 channels | the default 8->N matrices; channel 3 through the 120 Hz LFE low-pass |
| `limiter`, `gain` | a 0.9 sine through a mastering matrix of 2.0, then gain 0.5 | peak 1.0 after the limiter, then 0.5; meters 0.9 in, 1.0 out; state words 1 and 2 at 0 (spec 09 §4.4); I16 at 0.5 x 32767 |
| `limiter_attack` | DC 0.9 through a mastering matrix of 2.0 | the setup's limiter attacking in 1 ms (spec 09 §3.1): sample i at 1.8 (T + kA^(i+1) (1 - T)), T = 1/1.8, kA = exp(-1/48) |
| `filter_*` | DC 0.5 through a 1 kHz low-pass and high-pass before port 0 | 0.5 and 0 once settled |
| `meters` | -0.3 / 0.6 alternating, sampler meter in mode 1 then 2 | 0.6, then RMS 0.4743 |
| `works_*` | 2-channel voices in a sampler rack of 4 channel works | 0x804A0306 for the third; KILL frees them. PLAY on a voice without setup returns 0x804A0052 and reserves nothing (spec 09 §1) |
| `submixer_delay` | sampler, submixer (port delay 100) and mastering | DC from sample 103 (100 of delay after the sampler's 3) |

The open rows (`loops_loop_callback_repeats`, `meter_decoded_*`) are the
questions the spec leaves to the console. The **unconfirmed** cases are firmware behaviour rea read in
the disassembly that looks odd enough to check before relying on it; psport follows the reading.

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
